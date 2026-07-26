//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/InstallerStub.cpp
// Purpose: Generate complete Windows installer and uninstaller x86-64 stubs
//          using the InstallerStubGen instruction emitter.
//
// Key invariants:
//   - Windows x64 ABI: 32-byte shadow space, RCX/RDX/R8/R9 for args.
//   - Stack aligned to 16 bytes before every call instruction.
//   - Windows package overlays use stored bootstrap entries. The main install
//     payload may be a DEFLATE-compressed inner ZIP expanded through Windows'
//     built-in PowerShell archive support.
//   - All direct Win32 API calls go through the IAT. Compressed payload
//     expansion invokes Windows' System32 PowerShell explicitly.
//
// Ownership/Lifetime:
//   - Pure functions. No state.
//
// Links: InstallerStub.hpp, InstallerStubGen.hpp, WindowsPackageBuilder.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Windows installer and uninstaller bootstrap generation.
/// @details Package layout metadata is lowered into native x86-64/AArch64 code,
///          dialog resources, encoded transactional PowerShell backends, IAT
///          imports, registry operations, extraction loops, and cleanup logic.
///          Generated code follows the Windows ABI and performs no host-side
///          installation actions while packaging.

#include "InstallerStub.hpp"
#include "InstallerStubGen.hpp"
#include "InstallerStubGenA64.hpp"
#include "PkgGzip.hpp"
#include "PkgUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace zanna::pkg {

namespace {

constexpr uint32_t kSectionAlignment = 0x1000;
constexpr uint32_t kTextRVA = 0x1000;
constexpr uint32_t kRdataBaseRVA = 0x2000;

constexpr uint32_t kMaxPathChars = 32768;

constexpr uint32_t kGenericRead = 0x80000000u;
constexpr uint32_t kGenericWrite = 0x40000000u;
constexpr uint32_t kFileShareRead = 1u;
constexpr uint32_t kOpenExisting = 3u;
constexpr uint32_t kCreateAlways = 2u;
constexpr uint32_t kFileAttributeNormal = 0x80u;
constexpr uint32_t kMoveFileDelayUntilReboot = 0x00000004u;
constexpr uint32_t kFileOperationDelete = 0x0003u;
constexpr uint32_t kFileOperationSilentFlags = 0x0414u;
constexpr uint32_t kCreateNoWindow = 0x08000000u;
constexpr uint32_t kWaitInfinite = 0xFFFFFFFFu;
constexpr uint32_t kWaitTimeout = 258u;
constexpr uint32_t kRegNone = 0u;
constexpr uint32_t kRegSz = 1u;
constexpr uint32_t kRegExpandSz = 2u;
constexpr uint32_t kRegDword = 4u;
constexpr uint32_t kErrorFileNotFound = 2u;
constexpr uint32_t kErrorPathNotFound = 3u;
constexpr uint32_t kErrorDirNotEmpty = 145u;
constexpr uint32_t kErrorAlreadyExists = 183u;
constexpr uint32_t kErrorMoreData = 234u;
constexpr uint32_t kErrorInstallUserExit = 1602u;
constexpr uint32_t kInstallerCopyChunkBytes = 1024u * 1024u;
constexpr uint32_t kLocaleUserDefault = 0x0400u;
constexpr uint64_t kHkeyCurrentUser = 0x80000001ull;
constexpr uint64_t kHkeyLocalMachine = 0x80000002ull;

// Stack frame layout (negative offsets from RBP)
constexpr int32_t kSelfPathOff = -0x10000;
constexpr int32_t kInstallPathOff = -0x20000;
constexpr int32_t kDesktopPathOff = -0x30000;
constexpr int32_t kMenuPathOff = -0x40000;
constexpr int32_t kTempPathOff = -0x50000;
constexpr int32_t kUninstallPathOff = -0x60000;
constexpr int32_t kPathOriginalOff = -0x70000;
constexpr int32_t kPathExpectedOff = -0x80000;
constexpr int32_t kHFileOff = -0x80008;
constexpr int32_t kHOutOff = -0x80010;
constexpr int32_t kPFileBufOff = -0x80018;
constexpr int32_t kBytesReadOff = -0x80020;
constexpr int32_t kBytesWrittenOff = -0x80028;
constexpr int32_t kRegKeyOff = -0x80030;
constexpr int32_t kRemainingBytesOff = -0x80038;
constexpr int32_t kCurrentFileOffsetOff = -0x80040;
constexpr int32_t kCurrentChunkBytesOff = -0x80048;
constexpr int32_t kCrcOff = -0x80050;
constexpr int32_t kPathUpdatedOff = -0x80058;
constexpr int32_t kQuietModeOff = -0x80060;
constexpr int32_t kNoRestartModeOff = -0x80068;
constexpr int32_t kInitCommonControlsOff = -0x80078;
constexpr int32_t kFileOpStructOff = -0x800A0;
constexpr int32_t kCommandLineOff = -0x800B0;
constexpr int32_t kKnownFolderPtrOff = -0x800B8;
constexpr int32_t kComponentSelectionOff = -0x800C0;
constexpr int32_t kProgressHwndOff = -0x80100;
constexpr int32_t kProgressPathReadyOff = -0x80108;
constexpr int32_t kMessageStructOff = -0x80130;
constexpr int32_t kCommandBufferOff = -0x90000;
constexpr int32_t kStartupInfoOff = -0x90200;
constexpr int32_t kProcessInfoOff = -0x90280;
constexpr uint32_t kFrameSize = 0x90400;

constexpr uint32_t kDlgIdOk = 1;
constexpr uint32_t kDlgIdCancel = 2;
constexpr uint32_t kDlgIdLicense = 1001;
constexpr uint32_t kDlgIdAccept = 1002;
constexpr uint32_t kDlgIdBanner = 1006;
constexpr uint32_t kDlgIdProgressStatus = 1007;
constexpr uint32_t kDlgIdProgressBar = 1008;
constexpr uint32_t kDlgIdComponentBase = 1100;
constexpr uint32_t kWmClose = 0x0010;
constexpr uint32_t kWmNextDlgCtl = 0x0028;
constexpr uint32_t kWmDrawItem = 0x002B;
constexpr uint32_t kWmCommand = 0x0111;
constexpr uint32_t kWmInitDialog = 0x0110;
constexpr uint32_t kWmCtlColorEdit = 0x0133;
constexpr uint32_t kWmCtlColorButton = 0x0135;
constexpr uint32_t kWmCtlColorDialog = 0x0136;
constexpr uint32_t kWmCtlColorStatic = 0x0138;
constexpr uint32_t kBmGetCheck = 0x00F0;
constexpr uint32_t kBmSetCheck = 0x00F1;
constexpr uint32_t kBstChecked = 1;
constexpr uint32_t kPbmSetPos = 0x0402;
constexpr uint32_t kIccProgressClass = 0x00000020;
constexpr uint32_t kPmRemove = 0x0001;
constexpr uint32_t kSwShow = 5;
constexpr uint32_t kDcBrush = 18;
constexpr uint32_t kDwmUseImmersiveDarkModeBefore20H1 = 19;
constexpr uint32_t kDwmUseImmersiveDarkMode = 20;
constexpr uint32_t kDarkBackground = 0x001B1C07u;
constexpr uint32_t kDarkSurface = 0x00292B10u;
constexpr uint32_t kDarkAccent = 0x002AA6F0u;
constexpr uint32_t kDarkText = 0x00C8F1FFu;
constexpr uint32_t kDarkMutedText = 0x00BCC9AAu;
constexpr uint32_t kSrcCopy = 0x00CC0020u;
constexpr int32_t kDlgProcFrameSize = 0x88;
constexpr int32_t kDlgProcHwndOff = -0x08;
constexpr int32_t kDlgProcMsgOff = -0x10;
constexpr int32_t kDlgProcWparamOff = -0x18;
constexpr int32_t kDlgProcLparamOff = -0x20;
constexpr int32_t kDlgProcDarkModeOff = -0x28;

/// @brief Flat IAT slot indices for the Win32 APIs the installer stub imports.
/// @details Each value is the function's position across all imported DLLs and is
///          passed to InstallerStubGen::callIATSlot to emit a `call [rip+disp]`.
///          The order must match the import list built in buildInstallerStub.
enum InstallerIAT : uint32_t {
    kI_ExitProcess = 0,
    kI_GetModuleFileNameW = 1,
    kI_CreateFileW = 2,
    kI_ReadFile = 3,
    kI_WriteFile = 4,
    kI_CloseHandle = 5,
    kI_LocalAlloc = 6,
    kI_LocalFree = 7,
    kI_SetFilePointerEx = 8,
    kI_CreateDirectoryW = 9,
    kI_DeleteFileW = 10,
    kI_RemoveDirectoryW = 11,
    kI_lstrcpyW = 12,
    kI_lstrcatW = 13,
    kI_lstrlenW = 14,
    kI_lstrcmpW = 15,
    kI_lstrcmpiW = 16,
    kI_GetLastError = 17,
    kI_GetCommandLineW = 18,
    kI_SHGetKnownFolderPath = 19,
    kI_SHFileOperationW = 20,
    kI_RegCreateKeyW = 21,
    kI_RegSetValueExW = 22,
    kI_RegCloseKey = 23,
    kI_RegOpenKeyW = 24,
    kI_RegQueryValueExW = 25,
    kI_RegDeleteValueW = 26,
    kI_RegDeleteTreeW = 27,
    kI_MessageBoxW = 28,
    kI_SendMessageTimeoutW = 29,
    kI_RtlComputeCrc32 = 30,
    kI_StrStrIW = 31,
    kI_CoTaskMemFree = 32,
    kI_GetDateFormatW = 33,
    kI_GetSystemDirectoryW = 34,
    kI_CreateProcessW = 35,
    kI_WaitForSingleObject = 36,
    kI_GetExitCodeProcess = 37,
    kI_InitCommonControlsEx = 38,
    kI_DialogBoxIndirectParamW = 39,
    kI_EndDialog = 40,
    kI_GetDlgItem = 41,
    kI_SendMessageW = 42,
    kI_EnableWindow = 43,
    kI_SetEnvironmentVariableW = 44,
    kI_GetDlgCtrlID = 45,
    kI_SetWindowTheme = 46,
    kI_DwmSetWindowAttribute = 47,
    kI_GetStockObject = 48,
    kI_SetDCBrushColor = 49,
    kI_SetTextColor = 50,
    kI_SetBkColor = 51,
    kI_CreateDialogIndirectParamW = 52,
    kI_ShowWindow = 53,
    kI_UpdateWindow = 54,
    kI_DestroyWindow = 55,
    kI_PeekMessageW = 56,
    kI_TranslateMessage = 57,
    kI_DispatchMessageW = 58,
    kI_SetDlgItemTextW = 59,
    kI_SetWindowLongPtrW = 60,
    kI_GetWindowLongPtrW = 61,
    kI_GetTempPathW = 62,
    kI_GetPrivateProfileIntW = 63,
    kI_GetPrivateProfileStringW = 64,
    kI_StretchDIBits = 65,
    kI_Count, ///< Sentinel: total IAT slot count (must equal installerImports() function count).
};

/// @brief Flat IAT slot indices for the Win32 APIs the uninstaller stub imports.
/// @details Mirrors InstallerIAT for the (smaller) uninstaller import list; the
///          order must match the import list built in buildUninstallerStub.
enum UninstallerIAT : uint32_t {
    kU_ExitProcess = 0,
    kU_GetModuleFileNameW = 1,
    kU_DeleteFileW = 2,
    kU_RemoveDirectoryW = 3,
    kU_MoveFileExW = 4,
    kU_lstrcpyW = 5,
    kU_lstrcatW = 6,
    kU_lstrlenW = 7,
    kU_lstrcmpW = 8,
    kU_lstrcmpiW = 9,
    kU_GetLastError = 10,
    kU_GetCommandLineW = 11,
    kU_SHGetKnownFolderPath = 12,
    kU_RegOpenKeyW = 13,
    kU_RegCreateKeyW = 14,
    kU_RegCloseKey = 15,
    kU_RegQueryValueExW = 16,
    kU_RegDeleteValueW = 17,
    kU_RegDeleteTreeW = 18,
    kU_RegSetValueExW = 19,
    kU_MessageBoxW = 20,
    kU_SendMessageTimeoutW = 21,
    kU_StrStrIW = 22,
    kU_CoTaskMemFree = 23,
    kU_InitCommonControlsEx = 24,
    kU_DialogBoxIndirectParamW = 25,
    kU_EndDialog = 26,
    kU_GetDlgItem = 27,
    kU_SendMessageW = 28,
    kU_EnableWindow = 29,
    kU_GetSystemDirectoryW = 30,
    kU_CreateProcessW = 31,
    kU_WaitForSingleObject = 32,
    kU_GetExitCodeProcess = 33,
    kU_SetEnvironmentVariableW = 34,
    kU_CloseHandle = 35,
    kU_GetDlgCtrlID = 36,
    kU_SetWindowTheme = 37,
    kU_DwmSetWindowAttribute = 38,
    kU_GetStockObject = 39,
    kU_SetDCBrushColor = 40,
    kU_SetTextColor = 41,
    kU_SetBkColor = 42,
    kU_Count, ///< Sentinel: total IAT slot count (must equal uninstallerImports() function count).
};

/// @brief Validate that the flat function count in @p imports matches @p expectedSlots.
/// @details The IAT enum (InstallerIAT/UninstallerIAT) and the PEImport list are a
///          load-bearing pairing: codegen references imports by numeric slot
///          (callIATSlot), so a count mismatch means an enum entry or an import was
///          added without the other — which would silently call the wrong DLL
///          function. Caught here at packaging time rather than as a runtime crash.
/// @param imports Ordered per-DLL import groups to count.
/// @param expectedSlots Sentinel value from the matching generated-code IAT enum.
/// @param label Bootstrap label included in a mismatch error.
/// @throws std::runtime_error When flattened import count and enum slots differ.
inline void verifyImportSlotCount(const std::vector<PEImport> &imports,
                                  std::size_t expectedSlots,
                                  const char *label) {
    std::size_t total = 0;
    for (const PEImport &imp : imports)
        total += imp.functions.size();
    if (total != expectedSlots)
        throw std::runtime_error(std::string("packaging: ") + label + " IAT enum/import drift — " +
                                 std::to_string(total) + " imported functions vs " +
                                 std::to_string(expectedSlots) +
                                 " enum slots; keep the enum and the import list in sync");
}

/// @brief Return the ordered PEImport list for the installer PE.
/// Slot indices must match the InstallerIAT enum — the pairing is load-bearing
/// because the codegen references imports by numeric slot, not by name.
std::vector<PEImport> installerImports() {
    std::vector<PEImport> imports = {
        {"kernel32.dll",
         {"ExitProcess",
          "GetModuleFileNameW",
          "CreateFileW",
          "ReadFile",
          "WriteFile",
          "CloseHandle",
          "LocalAlloc",
          "LocalFree",
          "SetFilePointerEx",
          "CreateDirectoryW",
          "DeleteFileW",
          "RemoveDirectoryW",
          "lstrcpyW",
          "lstrcatW",
          "lstrlenW",
          "lstrcmpW",
          "lstrcmpiW",
          "GetLastError",
          "GetCommandLineW"}},
        {"shell32.dll", {"SHGetKnownFolderPath", "SHFileOperationW"}},
        {"advapi32.dll",
         {"RegCreateKeyW",
          "RegSetValueExW",
          "RegCloseKey",
          "RegOpenKeyW",
          "RegQueryValueExW",
          "RegDeleteValueW",
          "RegDeleteTreeW"}},
        {"user32.dll", {"MessageBoxW", "SendMessageTimeoutW"}},
        {"ntdll.dll", {"RtlComputeCrc32"}},
        {"shlwapi.dll", {"StrStrIW"}},
        {"ole32.dll", {"CoTaskMemFree"}},
        {"kernel32.dll",
         {"GetDateFormatW",
          "GetSystemDirectoryW",
          "CreateProcessW",
          "WaitForSingleObject",
          "GetExitCodeProcess"}},
        {"comctl32.dll", {"InitCommonControlsEx"}},
        {"user32.dll",
         {"DialogBoxIndirectParamW", "EndDialog", "GetDlgItem", "SendMessageW", "EnableWindow"}},
        {"kernel32.dll", {"SetEnvironmentVariableW"}},
        {"user32.dll", {"GetDlgCtrlID"}},
        {"uxtheme.dll", {"SetWindowTheme"}},
        {"dwmapi.dll", {"DwmSetWindowAttribute"}},
        {"gdi32.dll", {"GetStockObject", "SetDCBrushColor", "SetTextColor", "SetBkColor"}},
        {"user32.dll",
         {"CreateDialogIndirectParamW",
          "ShowWindow",
          "UpdateWindow",
          "DestroyWindow",
          "PeekMessageW",
          "TranslateMessage",
          "DispatchMessageW",
          "SetDlgItemTextW",
          "SetWindowLongPtrW",
          "GetWindowLongPtrW"}},
        {"kernel32.dll", {"GetTempPathW", "GetPrivateProfileIntW", "GetPrivateProfileStringW"}},
        {"gdi32.dll", {"StretchDIBits"}},
    };
    verifyImportSlotCount(imports, kI_Count, "installer");
    return imports;
}

/// @brief Return the ordered PEImport list for the uninstaller PE.
/// Slot indices must match the UninstallerIAT enum for the same reason as installerImports().
std::vector<PEImport> uninstallerImports() {
    std::vector<PEImport> imports = {
        {"kernel32.dll",
         {"ExitProcess",
          "GetModuleFileNameW",
          "DeleteFileW",
          "RemoveDirectoryW",
          "MoveFileExW",
          "lstrcpyW",
          "lstrcatW",
          "lstrlenW",
          "lstrcmpW",
          "lstrcmpiW",
          "GetLastError",
          "GetCommandLineW"}},
        {"shell32.dll", {"SHGetKnownFolderPath"}},
        {"advapi32.dll",
         {"RegOpenKeyW",
          "RegCreateKeyW",
          "RegCloseKey",
          "RegQueryValueExW",
          "RegDeleteValueW",
          "RegDeleteTreeW",
          "RegSetValueExW"}},
        {"user32.dll", {"MessageBoxW", "SendMessageTimeoutW"}},
        {"shlwapi.dll", {"StrStrIW"}},
        {"ole32.dll", {"CoTaskMemFree"}},
        {"comctl32.dll", {"InitCommonControlsEx"}},
        {"user32.dll",
         {"DialogBoxIndirectParamW", "EndDialog", "GetDlgItem", "SendMessageW", "EnableWindow"}},
        {"kernel32.dll",
         {"GetSystemDirectoryW",
          "CreateProcessW",
          "WaitForSingleObject",
          "GetExitCodeProcess",
          "SetEnvironmentVariableW",
          "CloseHandle"}},
        {"user32.dll", {"GetDlgCtrlID"}},
        {"uxtheme.dll", {"SetWindowTheme"}},
        {"dwmapi.dll", {"DwmSetWindowAttribute"}},
        {"gdi32.dll", {"GetStockObject", "SetDCBrushColor", "SetTextColor", "SetBkColor"}},
    };
    verifyImportSlotCount(imports, kU_Count, "uninstaller");
    return imports;
}

/// @brief Round `value` up to the nearest multiple of `alignment` (must be a power of two).
/// @param value Unsigned value to round upward.
/// @param alignment Nonzero power-of-two boundary.
/// @return Smallest aligned value greater than or equal to @p value.
/// @pre `value + alignment - 1` is representable as `uint32_t`.
uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/// @brief Resolve the sanitized leaf directory used as the install root.
/// @param layout Package metadata supplying the preferred directory name.
/// @return Normalized install directory leaf.
std::string installDirNameFor(const WindowsPackageLayout &layout);

/// @brief Build the scope-relative Add/Remove Programs registry key.
/// @param layout Package metadata supplying its stable registry identity.
/// @return Registry subkey beneath the uninstall key.
std::string uninstallKeyPathFor(const WindowsPackageLayout &layout);

/// @brief Build the sanitized identifier used in registry metadata.
/// @param layout Package metadata supplying identifier and fallback name.
/// @return Stable registry-safe identifier.
std::string registryIdFor(const WindowsPackageLayout &layout);

/// @brief Determine whether generated code must resolve a Start Menu path.
/// @param layout Package files, directories, and shortcut settings to inspect.
/// @return True when any generated operation targets the Start Menu root.
bool needsMenuPath(const WindowsPackageLayout &layout);

/// @brief Append a 16-bit little-endian field to dialog-template bytes.
/// @param out Dialog byte buffer to extend.
/// @param value Unsigned value to encode.
void appendDlgLE16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

/// @brief Append a 32-bit little-endian field to dialog-template bytes.
/// @param out Dialog byte buffer to extend.
/// @param value Unsigned value to encode.
void appendDlgLE32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

/// @brief Pad dialog-template bytes to a four-byte boundary.
/// @param out Dialog buffer extended with zero bytes when necessary.
void alignDialogDword(std::vector<uint8_t> &out) {
    while ((out.size() & 3u) != 0)
        out.push_back(0);
}

/// @brief Append a NUL-terminated UTF-16LE string to a dialog template.
/// @param out Dialog byte buffer to extend.
/// @param text UTF-8 text to transcode.
void appendDialogWideString(std::vector<uint8_t> &out, const std::string &text) {
    const auto encoded = utf8ToUtf16LEBytes(text, true);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

/// @brief Normalize line endings for Win32 multi-line dialog controls.
/// @param text UTF-8 text whose bare line feeds should become CRLF.
/// @return Owned text with each LF preceded by exactly one carriage return.
std::string normalizeDialogText(std::string text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        if (ch == '\n') {
            if (out.empty() || out.back() != '\r')
                out.push_back('\r');
            out.push_back('\n');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

/// @brief Encode arbitrary bytes using standard padded Base64.
/// @param bytes Input byte sequence.
/// @return RFC 4648-style Base64 text using `=` padding.
std::string base64Encode(const std::vector<uint8_t> &bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2u) / 3u) * 4u);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = (i + 1u < bytes.size()) ? bytes[i + 1u] : 0u;
        const uint32_t b2 = (i + 2u < bytes.size()) ? bytes[i + 2u] : 0u;
        const uint32_t triplet = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triplet >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triplet >> 12) & 0x3Fu]);
        out.push_back((i + 1u < bytes.size()) ? kAlphabet[(triplet >> 6) & 0x3Fu] : '=');
        out.push_back((i + 2u < bytes.size()) ? kAlphabet[triplet & 0x3Fu] : '=');
    }
    return out;
}

/// @brief Quote one literal for insertion into single-quoted PowerShell syntax.
/// @param text Literal text to protect.
/// @return Single-quoted expression with embedded apostrophes doubled.
std::string powershellSingleQuote(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 2u);
    out.push_back('\'');
    for (const char ch : text) {
        if (ch == '\'')
            out += "''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

/// @brief Render a C++ boolean as a PowerShell Boolean literal.
/// @param value Boolean value to translate.
/// @return Static `$true` or `$false` literal.
const char *powershellBool(bool value) {
    return value ? "$true" : "$false";
}

/// @brief Map an installation root to the compact PowerShell manifest code.
/// @param root Logical destination root.
/// @return Static one-character code: `I`, `D`, or `M`.
/// @note Unrecognized values fall back to the install-directory code.
const char *powershellRootCode(WindowsInstallRoot root) {
    switch (root) {
        case WindowsInstallRoot::DesktopDir:
            return "D";
        case WindowsInstallRoot::StartMenuDir:
            return "M";
        case WindowsInstallRoot::InstallDir:
        default:
            return "I";
    }
}

/// @brief Normalize a package-relative path for PowerShell on Windows.
/// @param path Path copied by value for in-place separator conversion.
/// @return Path with forward slashes replaced by backslashes.
std::string powershellRelPath(std::string path) {
    for (char &ch : path) {
        if (ch == '/')
            ch = '\\';
    }
    return path;
}

/// @brief Return the child-process environment variable carrying one component selection.
/// @param componentId Validated component identifier.
/// @return Uppercase environment name with hyphens translated to underscores.
std::string componentEnvironmentName(const std::string &componentId) {
    std::string name = "ZANNA_INSTALLER_COMPONENT_";
    for (char c : componentId) {
        if (c == '-')
            name.push_back('_');
        else
            name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return name;
}

/// @brief Emit compact PowerShell metadata for selected components and payload ownership.
/// @details Homogeneous directory subtrees become prefix rules instead of one hashtable entry per
///          file. This keeps the encoded Windows PowerShell command well below CreateProcessW's
///          command-line limit even when a component contains thousands of sample files.
/// @param ps PowerShell source stream to append to.
/// @param layout Component definitions and installed-file ownership metadata.
/// @param selectionsFromEnvironment Whether selection values come from native-host
///        environment variables instead of component defaults.
void appendPowerShellComponentMetadata(std::ostringstream &ps,
                                       const WindowsPackageLayout &layout,
                                       bool selectionsFromEnvironment) {
    ps << "$selected=@{}\n";
    for (const WindowsOptionalComponent &component : layout.optionalComponents) {
        ps << "$selected[" << powershellSingleQuote(component.id) << "]=";
        if (selectionsFromEnvironment) {
            ps << "($env:" << componentEnvironmentName(component.id) << " -ne '0')\n";
        } else {
            ps << powershellBool(component.defaultSelected) << "\n";
        }
    }
    ps << "function Enabled([string]$id){return [string]::IsNullOrEmpty($id) -or "
          "($selected.ContainsKey($id) -and $selected[$id])}\n";

    struct PayloadPath {
        std::string path;
        std::string componentId;
    };

    /// @brief Normalize a payload path for case-insensitive Windows comparison.
    /// @param path Payload path to normalize.
    /// @return Backslash-separated path with ASCII letters folded to lowercase.
    const auto lowercasePath = [](std::string path) {
        path = powershellRelPath(std::move(path));
        for (char &c : path)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return path;
    };
    std::vector<PayloadPath> payloadPaths;
    for (const WindowsPackageFileEntry &file : layout.installedFiles) {
        if (file.root == WindowsInstallRoot::InstallDir)
            payloadPaths.push_back({lowercasePath(file.relativePath), file.componentId});
    }

    std::vector<std::pair<std::string, std::string>> prefixRules;
    std::vector<PayloadPath> exactRules;
    for (const PayloadPath &owned : payloadPaths) {
        if (owned.componentId.empty())
            continue;
        std::string homogeneousPrefix;
        for (size_t slash = owned.path.find('\\'); slash != std::string::npos;
            slash = owned.path.find('\\', slash + 1)) {
            const std::string candidate = owned.path.substr(0, slash + 1);
            /// @brief Check whether a path prefix belongs exclusively to one component.
            /// @param file Payload file to compare with the candidate prefix.
            /// @return `true` when `file` is outside the prefix or has the owning component.
            const bool homogeneous =
                std::all_of(payloadPaths.begin(), payloadPaths.end(), [&](const PayloadPath &file) {
                    return file.path.rfind(candidate, 0) != 0 ||
                           file.componentId == owned.componentId;
                });
            if (homogeneous) {
                homogeneousPrefix = candidate;
                break;
            }
        }
        if (homogeneousPrefix.empty()) {
            exactRules.push_back(owned);
            continue;
        }
        const auto rule = std::make_pair(homogeneousPrefix, owned.componentId);
        if (std::find(prefixRules.begin(), prefixRules.end(), rule) == prefixRules.end())
            prefixRules.push_back(rule);
    }
    std::sort(prefixRules.begin(), prefixRules.end());
    /// @brief Order exact ownership rules deterministically by path and component identifier.
    /// @param lhs Left-hand ownership rule.
    /// @param rhs Right-hand ownership rule.
    /// @return `true` when `lhs` precedes `rhs`.
    std::sort(
        exactRules.begin(), exactRules.end(), [](const PayloadPath &lhs, const PayloadPath &rhs) {
            return std::tie(lhs.path, lhs.componentId) < std::tie(rhs.path, rhs.componentId);
        });

    ps << "$payloadComponentPrefixes=@(\n";
    for (const auto &[prefix, componentId] : prefixRules) {
        ps << ",@(" << powershellSingleQuote(prefix) << ',' << powershellSingleQuote(componentId)
           << ")\n";
    }
    ps << ")\n";
    ps << "$payloadComponents=@{}\n";
    for (const PayloadPath &file : exactRules) {
        ps << "$payloadComponents[" << powershellSingleQuote(file.path)
           << "]=" << powershellSingleQuote(file.componentId) << "\n";
    }
    ps << "function PayloadComponent([string]$path){$exact=$payloadComponents[$path];if($null "
          "-ne $exact){return [string]$exact};foreach($rule in $payloadComponentPrefixes){if("
          "$path.StartsWith($rule[0],[StringComparison]::OrdinalIgnoreCase)){return [string]"
          "$rule[1]}};return ''}\n";
}

/// @brief Serialize external shortcut ownership for upgrade conflict checks.
/// @details Newline separates records and `|` separates the root code from a relative path. Both
///          characters are invalid in Windows shortcut filenames after package validation, making
///          the registry value unambiguous without an additional escaping format.
/// @param layout Install-file metadata whose external-root entries should be recorded.
/// @return Newline-delimited root/path ownership records.
std::string externalOwnershipManifest(const WindowsPackageLayout &layout) {
    std::ostringstream out;
    for (const auto &file : layout.installFiles) {
        if (file.root == WindowsInstallRoot::InstallDir)
            continue;
        out << powershellRootCode(file.root) << '|' << powershellRelPath(file.relativePath) << '\n';
    }
    return out.str();
}

/// @brief Build a PowerShell expression joining a base variable and relative path.
/// @param baseVar Trusted PowerShell expression naming the base directory.
/// @param relativePath Package-relative path to quote and append.
/// @return @p baseVar unchanged for an empty path, otherwise a `Join-Path` expression.
std::string powershellPathJoinLiteral(const std::string &baseVar, const std::string &relativePath) {
    if (relativePath.empty())
        return baseVar;
    return "(Join-Path " + baseVar + " " + powershellSingleQuote(powershellRelPath(relativePath)) +
           ")";
}

/// @brief Return license text small enough for the ARM64 PowerShell bootstrap command line.
/// @details The native x64 wizard embeds the complete license in PE data. The current ARM64
///          bootstrap passes its PowerShell program through `-EncodedCommand`, whose Windows
///          command-line limit cannot carry a full GPL license. The complete license remains in
///          the installed payload; this concise notice prevents package generation from failing
///          until ARM64 uses the shared native wizard implementation.
/// @param layout Package layout containing the complete license text.
/// @return Original license up to the inline cap, otherwise a concise fallback notice.
std::string arm64BootstrapLicenseText(const WindowsPackageLayout &layout) {
    constexpr size_t kMaxInlineLicenseBytes = 2048;
    if (layout.licenseText.size() <= kMaxInlineLicenseBytes)
        return layout.licenseText;
    return "The complete license terms are included with this package and are installed with "
           "the application. Review the packaged LICENSE file before continuing.";
}

/// @brief Build the detached PowerShell cleanup command used by the ARM64 uninstaller.
/// @details The installed uninstaller cannot delete its own executable while the native parent
///          process is waiting for PowerShell. A detached child retries the self-delete after both
///          processes exit, then removes the install root only when it is empty.
/// @return UTF-16LE PowerShell cleanup source encoded for `-EncodedCommand`.
std::string arm64CleanupEncodedCommand() {
    const std::string cleanup =
        "$target=$args[0]\n"
        "$root=$args[1]\n"
        "for($i=0;$i -lt 100;$i++){Start-Sleep -Milliseconds 100;try{[IO.File]::Delete($target)}"
        "catch{};if(-not [IO.File]::Exists($target)){break}}\n"
        "try{[IO.Directory]::Delete($root,$false)}catch{}\n";
    return base64Encode(utf8ToUtf16LEBytes(cleanup, false));
}

/// @brief Append the shared PowerShell file transaction used by both Windows bootstraps.
/// @details The transaction stages and validates the compressed payload, rejects unowned path
///          collisions and reparse-point traversal, snapshots PATH/registry state, journals every
///          replacement, and retains its backup until the native x64 bootstrap explicitly commits.
///          ARM64 runs the same transaction and commits only after its metadata work succeeds.
/// @param ps PowerShell source stream to extend.
/// @param durableExtraction Whether staging must wait for explicit native commit
///        instead of committing during the script.
void appendWindowsInstallTransaction(std::ostringstream &ps, bool durableExtraction) {
    ps << R"ZANNAPS(
function RelOk([string]$s){
 if([string]::IsNullOrWhiteSpace($s) -or [IO.Path]::IsPathRooted($s) -or $s.Contains(':')){return $false}
 foreach($part in ($s -split '[\\/]+')){if($part -eq '' -or $part -eq '.' -or $part -eq '..'){return $false}}
 return $true
}
function NormRel([string]$s){if(-not (RelOk $s)){throw 'unsafe installer manifest path: '+$s};return $s.Replace('/','\')}
function OwnedPath([string]$r,[string]$rel){
 $root=Root $r;$clean=NormRel $rel;$base=[IO.Path]::GetFullPath($root);$full=[IO.Path]::GetFullPath((Join-Path $base $clean));
 $prefix=$base.TrimEnd('\')+'\';if(-not $full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'installer path escapes its root: '+$rel};return $full
}
function AssertSafe([string]$r,[string]$rel){
 $root=Root $r;$clean=NormRel $rel;if($r -eq 'I' -and (Test-Path -LiteralPath $root)){$a=(Get-Item -LiteralPath $root -Force).Attributes;if(($a -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'refusing reparse-point install root: '+$root}}
 $cur=[IO.Path]::GetFullPath($root);$parts=$clean -split '\\';for($i=0;$i -lt $parts.Length-1;$i++){$cur=Join-Path $cur $parts[$i];if(Test-Path -LiteralPath $cur){$a=(Get-Item -LiteralPath $cur -Force).Attributes;if(($a -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'refusing reparse-point installer parent: '+$cur}}}
 $dst=OwnedPath $r $clean;if(Test-Path -LiteralPath $dst){$item=Get-Item -LiteralPath $dst -Force;if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'refusing reparse-point installer destination: '+$dst};if($item.PSIsContainer){throw 'refusing to replace directory with file: '+$dst}}
 return $dst
}
function TxnOwned(){return (Test-Path -LiteralPath $txnMarker -PathType Leaf) -and ([IO.File]::ReadAllText($txnMarker) -eq $identifier)}
function Journal([string]$kind,[string]$r,[string]$rel,[string]$slot){Add-Content -LiteralPath $txnJournal -Value ($kind+'|'+$r+'|'+$rel+'|'+$slot) -Encoding UTF8}
function RestoreTransaction(){
 if(-not (Test-Path -LiteralPath $txn)){return};if(-not (TxnOwned)){throw 'refusing unowned Windows installer transaction: '+$txn}
 if(Test-Path -LiteralPath $txnJournal -PathType Leaf){$lines=@(Get-Content -LiteralPath $txnJournal);[array]::Reverse($lines);foreach($line in $lines){$p=$line -split '\|',4;if($p.Length -ne 4){continue};$dst=OwnedPath $p[1] $p[2];if($p[0] -eq 'B'){$src=Join-Path $txnBackup $p[3];if(Test-Path -LiteralPath $dst){Remove-Item -LiteralPath $dst -Force -ErrorAction Stop};if(Test-Path -LiteralPath $src -PathType Leaf){Parent $dst;Copy-Item -LiteralPath $src -Destination $dst -Force}}elseif($p[0] -eq 'N'){if(Test-Path -LiteralPath $dst -PathType Leaf){Remove-Item -LiteralPath $dst -Force -ErrorAction Stop}}}}
 $regDir=Join-Path $txn 'registry';if(Test-Path -LiteralPath $regDir -PathType Container){foreach($entry in Get-ChildItem -LiteralPath $regDir -Directory){$state=Join-Path $entry.FullName 'state';if(-not (Test-Path -LiteralPath $state -PathType Leaf)){continue};$provider=[IO.File]::ReadAllText((Join-Path $entry.FullName 'provider'));if(Test-Path -LiteralPath $provider){Remove-Item -LiteralPath $provider -Recurse -Force -ErrorAction Stop};if([IO.File]::ReadAllText($state) -eq 'present'){$regFile=Join-Path $entry.FullName 'backup.reg';& $regExe import $regFile | Out-Null;if($LASTEXITCODE -ne 0){throw 'failed to restore Windows installer registry state'}}}}
 $pathState=Join-Path $txn 'path.state';if(Test-Path -LiteralPath $pathState -PathType Leaf){$state=[IO.File]::ReadAllText($pathState);if($state -eq 'null'){[Environment]::SetEnvironmentVariable('Path',$null,$scope)}else{$bytes=[Convert]::FromBase64String($state);[Environment]::SetEnvironmentVariable('Path',[Text.Encoding]::UTF8.GetString($bytes),$scope)}}
 Remove-Item -LiteralPath $txn -Recurse -Force -ErrorAction Stop;BroadcastEnv
}
function CommitTransaction(){if(Test-Path -LiteralPath $txn){if(-not (TxnOwned)){throw 'refusing unowned Windows installer transaction: '+$txn};Remove-Item -LiteralPath $txn -Recurse -Force -ErrorAction Stop}}
function LoadManifest([string]$path,[bool]$required){
 if(-not (Test-Path -LiteralPath $path -PathType Leaf)){if($required){throw 'required installer manifest is missing: '+$path};return @()}
 $out=@();$seen=@{};foreach($raw in Get-Content -LiteralPath $path){if($raw -eq ''){continue};$rel=NormRel $raw;$key=$rel.ToLowerInvariant();if($seen.ContainsKey($key)){throw 'duplicate installer manifest path: '+$rel};$seen[$key]=$true;$out+=,$rel};return $out
}
function ContainsBytes([byte[]]$data,[byte[]]$needle){if($needle.Length -eq 0 -or $data.Length -lt $needle.Length){return $false};for($i=0;$i -le $data.Length-$needle.Length;$i++){$match=$true;for($j=0;$j -lt $needle.Length;$j++){if($data[$i+$j] -ne $needle[$j]){$match=$false;break}};if($match){return $true}};return $false}
function LegacyShortcutOwned([string]$path){if(-not $path.EndsWith('.lnk',[StringComparison]::OrdinalIgnoreCase)){return $false};try{$data=[IO.File]::ReadAllBytes($path);return (ContainsBytes $data ([Text.Encoding]::Unicode.GetBytes($installLeaf))) -or (ContainsBytes $data ([Text.Encoding]::UTF8.GetBytes($installLeaf)))}catch{return $false}}
function LegacyInstallOwned(){
 $path=Join-Path $install 'uninstall.exe';if(-not (Test-Path -LiteralPath $path -PathType Leaf)){return $false};try{$item=Get-Item -LiteralPath $path -Force;if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){return $false};$info=[Diagnostics.FileVersionInfo]::GetVersionInfo($path);if(-not [String]::Equals($info.OriginalFilename,'uninstall.exe',[StringComparison]::OrdinalIgnoreCase) -or -not $info.FileDescription.EndsWith(' Uninstaller',[StringComparison]::OrdinalIgnoreCase)){return $false};$data=[IO.File]::ReadAllBytes($path);return (ContainsBytes $data ([Text.Encoding]::Unicode.GetBytes($identifier))) -and (ContainsBytes $data ([Text.Encoding]::Unicode.GetBytes($installedManifestRel)))}catch{return $false}
}
function SnapshotState(){
 $pathValue=[Environment]::GetEnvironmentVariable('Path',$scope);$pathState=Join-Path $txn 'path.state';if($null -eq $pathValue){[IO.File]::WriteAllText($pathState,'null')}else{[IO.File]::WriteAllText($pathState,[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pathValue)))}
 $regDir=Join-Path $txn 'registry';[IO.Directory]::CreateDirectory($regDir)|Out-Null;$i=0;foreach($rk in $regKeys){$dir=Join-Path $regDir ([string]$i);[IO.Directory]::CreateDirectory($dir)|Out-Null;[IO.File]::WriteAllText((Join-Path $dir 'provider'),$rk[0]);[IO.File]::WriteAllText((Join-Path $dir 'native'),$rk[1]);if(Test-Path -LiteralPath $rk[0]){& $regExe export $rk[1] (Join-Path $dir 'backup.reg') /y | Out-Null;if($LASTEXITCODE -ne 0){throw 'failed to snapshot Windows installer registry state'};[IO.File]::WriteAllText((Join-Path $dir 'state'),'present')}else{[IO.File]::WriteAllText((Join-Path $dir 'state'),'absent')};$i++}
}
function CopyOverlay($fs,$f,[string]$dst){
 Parent $dst;$tmp=$dst+'.zanna-new-'+[Guid]::NewGuid().ToString('N');$out=[IO.File]::Open($tmp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);$hash=$null;$actual='';$left=[int64]$f[3];try{if($f[4]){$hash=[Security.Cryptography.SHA256]::Create()};$null=$fs.Seek($baseOffset+[int64]$f[2],[IO.SeekOrigin]::Begin);$buf=New-Object byte[] 65536;while($left -gt 0){$want=$buf.Length;if($left -lt $want){$want=[int]$left};$n=$fs.Read($buf,0,$want);if($n -le 0){throw 'installer overlay ended early'};if($hash){$null=$hash.TransformBlock($buf,0,$n,$buf,0)};$out.Write($buf,0,$n);$left-=$n};if($hash){$null=$hash.TransformFinalBlock([byte[]]@(),0,0);$actual=[BitConverter]::ToString($hash.Hash).Replace('-','').ToLowerInvariant()}}finally{$out.Dispose();if($hash){$hash.Dispose()};if($left -gt 0){Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue}};if($f[4] -and $actual -ne $f[4]){Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue;throw 'installer overlay checksum mismatch'};return $tmp
}
function ReplaceTemp([string]$tmp,[string]$dst){try{if(Test-Path -LiteralPath $dst){Remove-Item -LiteralPath $dst -Force -ErrorAction Stop};Move-Item -LiteralPath $tmp -Destination $dst -Force -ErrorAction Stop}finally{if(Test-Path -LiteralPath $tmp){Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue}}}
)ZANNAPS";
    if (durableExtraction) {
        ps << R"ZANNAPS(
function ReportProgress([int]$percent,[string]$status){
 if(-not $progressPath){return};if($percent -lt 0){$percent=0};if($percent -gt 100){$percent=100};if($script:lastProgress -eq $percent -and $script:lastStatus -eq $status){return};$script:lastProgress=$percent;$script:lastStatus=$status
 try{$safe=$status.Replace("`r",' ').Replace("`n",' ');[IO.File]::WriteAllText($progressPath,"[progress]`r`npercent=$percent`r`nstatus=$safe`r`n",(New-Object Text.UTF8Encoding($false)))}catch{}
}
function ExtractPayload([string]$zipPath,[string]$dstRoot){
 Add-Type -AssemblyName System.IO.Compression -ErrorAction Stop;Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
 $archive=[IO.Compression.ZipFile]::OpenRead($zipPath);try{$rootFull=[IO.Path]::GetFullPath($dstRoot).TrimEnd('\')+'\';$entries=@($archive.Entries|Where-Object{-not [string]::IsNullOrEmpty($_.Name)});[int64]$total=0;foreach($entry in $entries){$total+=[int64]$entry.Length};if($total -lt 1){$total=1};[int64]$done=0;$seen=@{}
  foreach($entry in $archive.Entries){$raw=$entry.FullName.Replace('/','\');if([string]::IsNullOrEmpty($entry.Name)){$dirRel=NormRel $raw.TrimEnd('\');$dir=[IO.Path]::GetFullPath((Join-Path $dstRoot $dirRel));if(-not $dir.StartsWith($rootFull,[StringComparison]::OrdinalIgnoreCase)){throw 'compressed installer directory escapes staging root: '+$raw};[IO.Directory]::CreateDirectory($dir)|Out-Null;continue};$rel=NormRel $raw;$key=$rel.ToLowerInvariant();if($seen.ContainsKey($key)){throw 'compressed installer payload contains a duplicate file: '+$rel};$seen[$key]=$true;$dst=[IO.Path]::GetFullPath((Join-Path $dstRoot $rel));if(-not $dst.StartsWith($rootFull,[StringComparison]::OrdinalIgnoreCase)){throw 'compressed installer file escapes staging root: '+$rel};Parent $dst;$input=$entry.Open();$output=[IO.File]::Open($dst,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);try{$buf=New-Object byte[] 131072;while(($n=$input.Read($buf,0,$buf.Length)) -gt 0){$output.Write($buf,0,$n);$done+=$n;ReportProgress (5+[int][Math]::Floor(55.0*$done/$total)) 'Extracting and verifying the Zanna toolchain...'}}finally{$output.Dispose();$input.Dispose()}}
 }finally{$archive.Dispose()}
}
)ZANNAPS";
    } else {
        ps << R"ZANNAPS(
function BeginTransaction(){
 if(Test-Path -LiteralPath $txn){$item=Get-Item -LiteralPath $txn -Force;if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'refusing reparse-point Windows installer transaction: '+$txn};RestoreTransaction}
 [IO.Directory]::CreateDirectory($txn)|Out-Null;[IO.File]::WriteAllText($txnMarker,$identifier);[IO.Directory]::CreateDirectory($txnBackup)|Out-Null;SnapshotState
 $oldPath=Join-Path $install $installedManifestRel;$hadOld=Test-Path -LiteralPath $oldPath -PathType Leaf;$legacyOwned=(-not $hadOld) -and (LegacyInstallOwned);if($legacyOwned){WriteInstallerLog 'INFO' ('migrating generated legacy installation at '+$install)};$oldLines=@(LoadManifest $oldPath $false);$oldSet=@{};foreach($o in $oldLines){$oldSet[$o.ToLowerInvariant()]=$true};$oldShortcutProps=Get-ItemProperty -LiteralPath $uninstallReg -ErrorAction SilentlyContinue;$hasOldShortcutMarker=$null -ne $oldShortcutProps -and $oldShortcutProps.PSObject.Properties.Name -contains 'ZAPSShortcutPaths';$oldShortcutText=$oldShortcutProps.ZAPSShortcutPaths;$oldShortcuts=@{};if($oldShortcutText){foreach($line in $oldShortcutText -split "`n"){$line=$line.TrimEnd("`r");if($line){$oldShortcuts[$line.ToLowerInvariant()]=$true}}}
 $items=@();$newLines=@();$newSet=@{};$stage=Join-Path $txn 'stage';[IO.Directory]::CreateDirectory($stage)|Out-Null;$fs=[IO.File]::OpenRead($self)
 try{
  if($payloadRel){$payloadRecord=$files|Where-Object{$_.Count -ge 5 -and $_[0] -eq 'I' -and [String]::Equals($_[1],$payloadRel,'OrdinalIgnoreCase')}|Select-Object -First 1;$manifestRecord=$files|Where-Object{$_.Count -ge 5 -and $_[0] -eq 'I' -and [String]::Equals($_[1],$nextManifestRel,'OrdinalIgnoreCase')}|Select-Object -First 1;if(-not $payloadRecord -or -not $manifestRecord){throw 'compressed installer bootstrap records are missing'};$payloadInput=Join-Path $txn 'payload.zip';$manifestInput=Join-Path $txn 'next.manifest';ReplaceTemp (CopyOverlay $fs $payloadRecord $payloadInput) $payloadInput;ReplaceTemp (CopyOverlay $fs $manifestRecord $manifestInput) $manifestInput;$newLines=@(LoadManifest $manifestInput $true);foreach($n in $newLines){$newSet[$n.ToLowerInvariant()]=$true};Expand-Archive -LiteralPath $payloadInput -DestinationPath $stage -Force
   foreach($rp in Get-ChildItem -LiteralPath $stage -Recurse -Force|Where-Object{($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0}){throw 'compressed installer payload contains a reparse point: '+$rp.FullName}
   $actual=@{};foreach($sf in Get-ChildItem -LiteralPath $stage -Recurse -File -Force){$rel=$sf.FullName.Substring($stage.TrimEnd('\').Length).TrimStart('\');$rel=NormRel $rel;$actual[$rel.ToLowerInvariant()]=$true;if(-not $newSet.ContainsKey($rel.ToLowerInvariant())){throw 'compressed installer payload contains an unmanifested file: '+$rel}}
   foreach($n in $newLines){if(-not $actual.ContainsKey($n.ToLowerInvariant())){throw 'compressed installer payload is missing a manifest file: '+$n};$items+=,@{R='I';P=$n;S=(Join-Path $stage $n);F=$null}}
  }
  foreach($f in $files){if($payloadRel -and $f[0] -eq 'I' -and ([String]::Equals($f[1],$payloadRel,'OrdinalIgnoreCase') -or [String]::Equals($f[1],$nextManifestRel,'OrdinalIgnoreCase'))){continue};$items+=,@{R=$f[0];P=(NormRel $f[1]);S=$null;F=$f}}
  $destSeen=@{};foreach($item in $items){$dst=AssertSafe $item.R $item.P;$key=($item.R+'|'+$item.P).ToLowerInvariant();if($destSeen.ContainsKey($key)){throw 'duplicate Windows installer destination: '+$item.P};$destSeen[$key]=$true;if(Test-Path -LiteralPath $dst){if($item.R -eq 'I'){$owned=$oldSet.ContainsKey($item.P.ToLowerInvariant()) -or $legacyOwned}else{$owned=($hadOld -or $legacyOwned) -and ($oldShortcuts.ContainsKey($key) -or ((-not $hasOldShortcutMarker) -and (LegacyShortcutOwned $dst)))};if(-not $owned){throw 'refusing to replace unowned path: '+$dst}}}
  foreach($o in $oldLines){if(-not $newSet.ContainsKey($o.ToLowerInvariant())){$null=AssertSafe 'I' $o}}
  $slot=0;foreach($item in $items){$dst=OwnedPath $item.R $item.P;if(Test-Path -LiteralPath $dst -PathType Leaf){$backup=[string]$slot;Copy-Item -LiteralPath $dst -Destination (Join-Path $txnBackup $backup) -Force;Journal 'B' $item.R $item.P $backup;$slot++}else{Journal 'N' $item.R $item.P ''}}
  foreach($o in $oldLines){if(-not $newSet.ContainsKey($o.ToLowerInvariant())){$dst=OwnedPath 'I' $o;if(Test-Path -LiteralPath $dst -PathType Leaf){$backup=[string]$slot;Copy-Item -LiteralPath $dst -Destination (Join-Path $txnBackup $backup) -Force;Journal 'B' 'I' $o $backup;$slot++;Remove-Item -LiteralPath $dst -Force -ErrorAction Stop}}}
  foreach($item in $items){$dst=AssertSafe $item.R $item.P;Parent $dst;if($item.S){$tmp=$dst+'.zanna-new-'+[Guid]::NewGuid().ToString('N');Copy-Item -LiteralPath $item.S -Destination $tmp -Force;ReplaceTemp $tmp $dst}else{ReplaceTemp (CopyOverlay $fs $item.F $dst) $dst}}
 }finally{$fs.Dispose()}
}
)ZANNAPS";
        return;
    }
    ps << R"ZANNAPS(
function BeginTransaction(){
 if(Test-Path -LiteralPath $txn){$item=Get-Item -LiteralPath $txn -Force;if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'refusing reparse-point Windows installer transaction: '+$txn};RestoreTransaction}
 ReportProgress 4 'Preparing a recoverable installation transaction...';[IO.Directory]::CreateDirectory($txn)|Out-Null;[IO.File]::WriteAllText($txnMarker,$identifier);[IO.Directory]::CreateDirectory($txnBackup)|Out-Null;SnapshotState
 $oldPath=Join-Path $install $installedManifestRel;$hadOld=Test-Path -LiteralPath $oldPath -PathType Leaf;$legacyOwned=(-not $hadOld) -and (LegacyInstallOwned);if($legacyOwned){WriteInstallerLog 'INFO' ('migrating generated legacy installation at '+$install)};$oldLines=@(LoadManifest $oldPath $false);$oldSet=@{};foreach($o in $oldLines){$oldSet[$o.ToLowerInvariant()]=$true};$oldShortcutProps=Get-ItemProperty -LiteralPath $uninstallReg -ErrorAction SilentlyContinue;$hasOldShortcutMarker=$null -ne $oldShortcutProps -and $oldShortcutProps.PSObject.Properties.Name -contains 'ZAPSShortcutPaths';$oldShortcutText=$oldShortcutProps.ZAPSShortcutPaths;$oldShortcuts=@{};$oldShortcutLines=@();if($oldShortcutText){foreach($line in $oldShortcutText -split "`n"){$line=$line.TrimEnd("`r");if($line){$oldShortcuts[$line.ToLowerInvariant()]=$true;$oldShortcutLines+=,$line}}}elseif($legacyOwned){foreach($f in $files){if($f[0] -eq 'I'){continue};$line=$f[0]+'|'+(NormRel $f[1]);$p=$line -split '\|',2;$dst=OwnedPath $p[0] $p[1];if((Test-Path -LiteralPath $dst -PathType Leaf) -and (LegacyShortcutOwned $dst)){$oldShortcuts[$line.ToLowerInvariant()]=$true;$oldShortcutLines+=,$line}}}
 $items=@();$newLines=@();$newSet=@{};$stage=Join-Path $txn 'stage';[IO.Directory]::CreateDirectory($stage)|Out-Null;$fs=[IO.File]::OpenRead($self)
 try{
  if($payloadRel){$payloadRecord=$files|Where-Object{$_.Count -ge 5 -and $_[0] -eq 'I' -and [String]::Equals($_[1],$payloadRel,'OrdinalIgnoreCase')}|Select-Object -First 1;$manifestRecord=$files|Where-Object{$_.Count -ge 5 -and $_[0] -eq 'I' -and [String]::Equals($_[1],$nextManifestRel,'OrdinalIgnoreCase')}|Select-Object -First 1;if(-not $payloadRecord -or -not $manifestRecord){throw 'compressed installer bootstrap records are missing'};$payloadInput=Join-Path $txn 'payload.zip';$manifestInput=Join-Path $txn 'next.manifest';ReplaceTemp (CopyOverlay $fs $payloadRecord $payloadInput) $payloadInput;ReplaceTemp (CopyOverlay $fs $manifestRecord $manifestInput) $manifestInput;$allLines=@(LoadManifest $manifestInput $true);$allSet=@{};foreach($n in $allLines){$allSet[$n.ToLowerInvariant()]=$true};ExtractPayload $payloadInput $stage
   foreach($rp in Get-ChildItem -LiteralPath $stage -Recurse -Force|Where-Object{($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0}){throw 'compressed installer payload contains a reparse point: '+$rp.FullName}
   $actual=@{};foreach($sf in Get-ChildItem -LiteralPath $stage -Recurse -File -Force){$rel=$sf.FullName.Substring($stage.TrimEnd('\').Length).TrimStart('\');$rel=NormRel $rel;$actual[$rel.ToLowerInvariant()]=$true;if(-not $allSet.ContainsKey($rel.ToLowerInvariant())){throw 'compressed installer payload contains an unmanifested file: '+$rel}}
   foreach($n in $allLines){$key=$n.ToLowerInvariant();if(-not $actual.ContainsKey($key)){throw 'compressed installer payload is missing a manifest file: '+$n};$component=PayloadComponent $key;if(Enabled $component){$newLines+=,$n;$newSet[$key]=$true}}
   $manifestStage=Join-Path $stage $installedManifestRel;if(-not $newSet.ContainsKey($installedManifestRel.ToLowerInvariant())){throw 'selected payload omitted its ownership manifest'};[IO.File]::WriteAllText($manifestStage,(($newLines -join "`r`n")+"`r`n"),(New-Object Text.UTF8Encoding($false)))
   foreach($n in $newLines){$items+=,@{R='I';P=$n;S=(Join-Path $stage $n);F=$null}}
  }
  foreach($f in $files){if($payloadRel -and $f[0] -eq 'I' -and ([String]::Equals($f[1],$payloadRel,'OrdinalIgnoreCase') -or [String]::Equals($f[1],$nextManifestRel,'OrdinalIgnoreCase'))){continue};if($f.Count -ge 6 -and -not (Enabled ([string]$f[5]))){continue};$items+=,@{R=$f[0];P=(NormRel $f[1]);S=$null;F=$f}}
  $newExternal=@{};foreach($item in $items){if($item.R -ne 'I'){$newExternal[($item.R+'|'+$item.P).ToLowerInvariant()]=$true}}
  $destSeen=@{};foreach($item in $items){$dst=AssertSafe $item.R $item.P;$key=($item.R+'|'+$item.P).ToLowerInvariant();if($destSeen.ContainsKey($key)){throw 'duplicate Windows installer destination: '+$item.P};$destSeen[$key]=$true;if(Test-Path -LiteralPath $dst){if($item.R -eq 'I'){$owned=$oldSet.ContainsKey($item.P.ToLowerInvariant()) -or $legacyOwned}else{$owned=($hadOld -or $legacyOwned) -and ($oldShortcuts.ContainsKey($key) -or ((-not $hasOldShortcutMarker) -and (LegacyShortcutOwned $dst)))};if(-not $owned){throw 'refusing to replace unowned path: '+$dst}}}
  foreach($o in $oldLines){if(-not $newSet.ContainsKey($o.ToLowerInvariant())){$null=AssertSafe 'I' $o}}
  foreach($line in $oldShortcutLines){if(-not $newExternal.ContainsKey($line.ToLowerInvariant())){$p=$line -split '\|',2;if($p.Length -eq 2){$null=AssertSafe $p[0] $p[1]}}}
  $slot=0;foreach($item in $items){$dst=OwnedPath $item.R $item.P;if(Test-Path -LiteralPath $dst -PathType Leaf){$backup=[string]$slot;Copy-Item -LiteralPath $dst -Destination (Join-Path $txnBackup $backup) -Force;Journal 'B' $item.R $item.P $backup;$slot++}else{Journal 'N' $item.R $item.P ''}}
  foreach($o in $oldLines){if(-not $newSet.ContainsKey($o.ToLowerInvariant())){$dst=OwnedPath 'I' $o;if(Test-Path -LiteralPath $dst -PathType Leaf){$backup=[string]$slot;Copy-Item -LiteralPath $dst -Destination (Join-Path $txnBackup $backup) -Force;Journal 'B' 'I' $o $backup;$slot++;Remove-Item -LiteralPath $dst -Force -ErrorAction Stop}}}
  foreach($line in $oldShortcutLines){if(-not $newExternal.ContainsKey($line.ToLowerInvariant())){$p=$line -split '\|',2;if($p.Length -eq 2){$dst=OwnedPath $p[0] $p[1];if(Test-Path -LiteralPath $dst -PathType Leaf){$backup=[string]$slot;Copy-Item -LiteralPath $dst -Destination (Join-Path $txnBackup $backup) -Force;Journal 'B' $p[0] $p[1] $backup;$slot++;Remove-Item -LiteralPath $dst -Force -ErrorAction Stop}}}}
  ReportProgress 72 'Installing the selected developer tools...';$itemIndex=0;$itemCount=[Math]::Max(1,$items.Count);foreach($item in $items){$dst=AssertSafe $item.R $item.P;Parent $dst;if($item.S){$tmp=$dst+'.zanna-new-'+[Guid]::NewGuid().ToString('N');Copy-Item -LiteralPath $item.S -Destination $tmp -Force;ReplaceTemp $tmp $dst}else{ReplaceTemp (CopyOverlay $fs $item.F $dst) $dst};$itemIndex++;ReportProgress (72+[int][Math]::Floor(20.0*$itemIndex/$itemCount)) 'Installing the selected developer tools...'}
 }finally{$fs.Dispose()}
}
)ZANNAPS";
}

/// @brief Build the full PowerShell backend used by the AArch64 bootstrap.
/// @param layout Package metadata embedded as script literals and record arrays.
/// @param uninstallDialog Whether to emit uninstall rather than install control flow.
/// @return UTF-8 PowerShell source implementing UI, transactions, metadata, and cleanup.
/// @details The AArch64 native stub delegates most installer behavior to this
///          script while passing mode and selection state through environment variables.
std::string buildArm64PowerShellScript(const WindowsPackageLayout &layout, bool uninstallDialog) {
    const std::string installDir = installDirNameFor(layout);
    const std::string version = layout.version.empty() ? "0.0.0" : layout.version;
    const std::string publisher = layout.publisher.empty() ? "Zanna" : layout.publisher;
    const std::string uninstallKey = uninstallKeyPathFor(layout);
    const std::string hive = layout.perUserInstall ? "HKCU:" : "HKLM:";
    const std::string nativeHive = layout.perUserInstall ? "HKCU" : "HKLM";
    const std::string scope = layout.perUserInstall ? "User" : "Machine";
    const std::string iconRel = layout.displayIconRelativePath.empty()
                                    ? layout.executableName
                                    : layout.displayIconRelativePath;
    const std::string pathEntryExpr =
        layout.pathRelativePath.empty()
            ? "$install"
            : powershellPathJoinLiteral("$install", layout.pathRelativePath);
    const std::string assocExeRel = layout.fileAssociationExecutableRelativePath.empty()
                                        ? layout.executableName
                                        : layout.fileAssociationExecutableRelativePath;
    const std::string assocIconRel =
        layout.displayIconRelativePath.empty() ? assocExeRel : layout.displayIconRelativePath;
    const std::string shortcutOwnership = externalOwnershipManifest(layout);
    std::ostringstream ps;
    ps << "$ErrorActionPreference='Stop'\n";
    ps << "$self=$env:ZANNA_INSTALLER_SELF\n";
    ps << "$mode=$env:ZANNA_INSTALLER_MODE\n";
    ps << "$defaultLog=Join-Path ([IO.Path]::GetTempPath()) "
       << powershellSingleQuote("ZannaInstaller-" + registryIdFor(layout) + ".log") << "\n";
    ps << "$log=if($env:ZANNA_INSTALLER_LOG){[Environment]::ExpandEnvironmentVariables($env:"
          "ZANNA_INSTALLER_LOG)}else{$defaultLog}\n";
    ps << "function WriteInstallerLog([string]$level,[string]$message){try{$line=(Get-Date "
          "-Format 'o')+' ['+$level+'] '+$message+[Environment]::NewLine;[IO.File]::"
          "AppendAllText($log,$line,[Text.Encoding]::UTF8)}catch{}}\n";
    ps << "try{if($mode -eq 'install' -or $mode -eq 'install-files'){[IO.File]::WriteAllText("
          "$log,'',[Text.Encoding]::UTF8)};WriteInstallerLog 'INFO' ('backend mode: '+$mode)}"
          "catch{}\n";
    ps << "if($mode -eq 'rollback' -and $env:ZANNA_INSTALLER_NATIVE_STAGE){WriteInstallerLog "
          "'ERROR' ('native setup failed during '+$env:ZANNA_INSTALLER_NATIVE_STAGE)}\n";
    ps << "trap{$failure=$_;WriteInstallerLog 'ERROR' ('setup failed: '+$failure.Exception."
          "Message);try{[Console]::Error.WriteLine('setup failed: '+$failure.Exception.Message)}"
          "catch{};exit 1}\n";
    ps << "if(-not $self -or -not (Test-Path -LiteralPath $self -PathType Leaf)){throw 'installer "
          "bootstrap path was not provided'}\n";
    ps << "$display=" << powershellSingleQuote(layout.displayName) << "\n";
    ps << "$license=" << powershellSingleQuote(arm64BootstrapLicenseText(layout)) << "\n";
    ps << "$wizardSummary=" << powershellSingleQuote(layout.wizardSummary) << "\n";
    ps << "$installLeaf=" << powershellSingleQuote(installDir) << "\n";
    ps << "$version=" << powershellSingleQuote(version) << "\n";
    ps << "$publisher=" << powershellSingleQuote(publisher) << "\n";
    ps << "$homepage=" << powershellSingleQuote(layout.homepage) << "\n";
    ps << "$comments=" << powershellSingleQuote(layout.description) << "\n";
    ps << "$contact=" << powershellSingleQuote(layout.contact) << "\n";
    ps << "$identifier=" << powershellSingleQuote(registryIdFor(layout)) << "\n";
    ps << "$perUser=" << powershellBool(layout.perUserInstall) << "\n";
    ps << "$addPath=" << powershellBool(layout.addToPath) << "\n";
    ps << "$quiet=($env:ZANNA_INSTALLER_QUIET -eq '1')\n";
    ps << "$noRestart=($env:ZANNA_INSTALLER_NORESTART -eq '1')\n";
    if (uninstallDialog)
        ps << "$cleanupCommand=" << powershellSingleQuote(arm64CleanupEncodedCommand()) << "\n";
    ps << "$baseOffset=[int64]" << layout.overlayFileOffset << "\n";
    ps << "$rootBase=if($perUser){[Environment]::GetFolderPath('LocalApplicationData')}else{["
          "Environment]::GetFolderPath('ProgramFiles')}\n";
    ps << "$install=Join-Path $rootBase $installLeaf\n";
    ps << "$desktop=if($perUser){[Environment]::GetFolderPath('Desktop')}else{[Environment]::"
          "GetFolderPath('CommonDesktopDirectory')}\n";
    ps << "$programs=if($perUser){[Environment]::GetFolderPath('Programs')}else{[Environment]::"
          "GetFolderPath('CommonPrograms')}\n";
    ps << "$menu=Join-Path $programs $installLeaf\n";
    ps << "$scope=" << powershellSingleQuote(scope) << "\n";
    ps << "$uninstallReg=" << powershellSingleQuote(hive + "\\" + uninstallKey) << "\n";
    ps << "$classRoot=Join-Path " << powershellSingleQuote(hive) << " 'Software\\Classes'\n";
    ps << "$regExe=Join-Path $env:SystemRoot 'System32\\reg.exe'\n";
    ps << "$payloadRel="
       << powershellSingleQuote(powershellRelPath(layout.compressedPayloadRelativePath)) << "\n";
    ps << "$nextManifestRel="
       << powershellSingleQuote(powershellRelPath(layout.compressedPayloadManifestRelativePath))
       << "\n";
    ps << "$installedManifestRel="
       << powershellSingleQuote(powershellRelPath(layout.installedManifestRelativePath)) << "\n";
    ps << "$shortcutOwnership=" << powershellSingleQuote(shortcutOwnership) << "\n";
    ps << "$txn=$install+'.zanna-transaction'\n";
    ps << "$txnMarker=Join-Path $txn 'owner'\n";
    ps << "$txnJournal=Join-Path $txn 'journal'\n";
    ps << "$txnBackup=Join-Path $txn 'backup'\n";
    ps << "function Root($r){if($r -eq 'D'){$desktop}elseif($r -eq 'M'){$menu}else{$install}}\n";
    ps << "function "
          "Parent($p){$d=[IO.Path]::GetDirectoryName($p);if($d){[IO.Directory]::CreateDirectory($d)"
          "|Out-Null}}\n";
    ps << "function CopyPart($fs,$r,$rel,[int64]$off,[int64]$len,[string]$expected){$dst="
          "Join-Path (Root $r) $rel;Parent "
          "$dst;$out=[IO.File]::Create($dst);$hash=$null;$actual='';"
          "if($expected){$hash=[Security.Cryptography.SHA256]::Create()}try{$null=$fs.Seek("
          "$baseOffset+$off,[IO.SeekOrigin]::Begin);$buf=New-Object byte[] 65536;while($len -gt "
          "0){$want=$buf.Length;if($len -lt $want){$want=[int]$len};$n=$fs.Read($buf,0,$want);"
          "if($n -le 0){throw 'installer overlay ended "
          "early'};if($hash){$null=$hash.TransformBlock("
          "$buf,0,$n,$buf,0)};$out.Write($buf,0,$n);$len-=$n};if($hash){$null=$hash."
          "TransformFinalBlock([byte[]]@(),0,0);$actual=[BitConverter]::ToString($hash.Hash)."
          "Replace("
          "'-','').ToLowerInvariant()}}finally{$out.Dispose();if($hash){$hash.Dispose()}};if("
          "$expected -and $actual -ne $expected){[IO.File]::Delete($dst);throw 'installer overlay "
          "checksum mismatch'}}\n";
    ps << "function SetS($n,$v){if($null -ne $v -and $v.Length -gt 0){New-ItemProperty -Path "
          "$uninstallReg -Name $n -Value $v -PropertyType String -Force|Out-Null}}\n";
    ps << "function SetD($n,[int]$v){New-ItemProperty -Path $uninstallReg -Name $n -Value $v "
          "-PropertyType DWord -Force|Out-Null}\n";
    ps << "function AddPath($p){if(-not "
          "$addPath){return};$old=[Environment]::GetEnvironmentVariable('Path',$scope);if($null "
          "-eq $old){$old=''};$parts=@($old -split ';'|Where-Object{$_.Length -gt 0});$owned=-not "
          "($parts|Where-Object{[String]::Equals($_,$p,'OrdinalIgnoreCase')});if($owned){"
          "[Environment]::SetEnvironmentVariable('Path',(($parts+$p)-join ';'),$scope);SetS "
          "'ZAPSOriginalPath' $old;SetS 'ZAPSPathEntry' $p}}\n";
    ps << "function RemovePath(){if(-not $addPath){return};$entry=(Get-ItemProperty -Path "
          "$uninstallReg -Name 'ZAPSPathEntry' -ErrorAction "
          "SilentlyContinue).ZAPSPathEntry;if(-not $entry){return};$old=[Environment]::"
          "GetEnvironmentVariable('Path',$scope);if($null -eq "
          "$old){return};$parts=@($old -split ';'|Where-Object{$_.Length -gt 0 -and -not "
          "[String]::Equals($_,$entry,'OrdinalIgnoreCase')});[Environment]::SetEnvironmentVariable("
          "'Path',($parts -join ';'),$scope)}\n";
    ps << "function BroadcastEnv(){try{Add-Type -TypeDefinition 'using System; using "
          "System.Runtime.InteropServices; public static class ZannaEnv { "
          "[DllImport(\"user32.dll\", "
          "SetLastError=true, CharSet=CharSet.Auto)] public static extern IntPtr "
          "SendMessageTimeout("
          "IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint flags, uint timeout, out "
          "UIntPtr "
          "result); }' -ErrorAction SilentlyContinue|Out-Null;$r=[UIntPtr]::Zero;[void][ZannaEnv]::"
          "SendMessageTimeout([IntPtr]0xffff,0x1a,[UIntPtr]::Zero,'Environment',2,5000,[ref]$r)}"
          "catch{}}\n";
    ps << "function TrimDialog([string]$s){if(-not $s){return ''};if($s.Length -le 3800){return "
          "$s};return $s.Substring(0,3800)+\"`r`n`r`n...\"}\n";
    ps << "function ConfirmDialog([string]$title,[string]$message){if($quiet){return};try{Add-Type "
          "-AssemblyName System.Windows.Forms -ErrorAction Stop;$r=[System.Windows.Forms."
          "MessageBox]::Show($message,$title,[System.Windows.Forms.MessageBoxButtons]::OKCancel,"
          "[System.Windows.Forms.MessageBoxIcon]::Information);if($r -ne [System.Windows.Forms."
          "DialogResult]::OK){exit 1602}}catch{throw 'Unable to display the installer confirmation "
          "dialog: '+$_.Exception.Message}}\n";
    ps << "function RemoveOne($r,$rel){$p=Join-Path (Root $r) $rel;Remove-Item -LiteralPath $p "
          "-Force -ErrorAction SilentlyContinue}\n";
    ps << "$assocExe=" << powershellPathJoinLiteral("$install", assocExeRel) << "\n";
    ps << "$assocIcon=" << powershellPathJoinLiteral("$install", assocIconRel) << "\n";
    ps << "$assocs=@(\n";
    for (const auto &assoc : layout.fileAssociations) {
        ps << "@{Ext=" << powershellSingleQuote(assoc.extension)
           << ";Desc=" << powershellSingleQuote(assoc.description)
           << ";Mime=" << powershellSingleQuote(assoc.mimeType)
           << ";Prog=" << powershellSingleQuote(assoc.progId)
           << ";Args=" << powershellSingleQuote(assoc.openCommandArguments) << "}\n";
    }
    ps << ")\n";
    ps << "$regKeys=@(\n";
    ps << ",@(" << powershellSingleQuote(hive + "\\" + uninstallKey) << ","
       << powershellSingleQuote(nativeHive + "\\" + uninstallKey) << ")\n";
    for (const auto &assoc : layout.fileAssociations) {
        const std::string extensionKey = "Software\\Classes\\" + assoc.extension;
        const std::string progIdKey = "Software\\Classes\\" + assoc.progId;
        ps << ",@(" << powershellSingleQuote(hive + "\\" + extensionKey) << ","
           << powershellSingleQuote(nativeHive + "\\" + extensionKey) << ")\n";
        ps << ",@(" << powershellSingleQuote(hive + "\\" + progIdKey) << ","
           << powershellSingleQuote(nativeHive + "\\" + progIdKey) << ")\n";
    }
    ps << ")\n";
    appendWindowsInstallTransaction(ps, false);
    ps << "function RegDefault($path,$value){New-Item -Path $path -Force|Out-Null;Set-Item -Path "
          "$path -Value $value}\n";
    ps << "function RegisterAssoc(){foreach($a in $assocs){$ext=Join-Path $classRoot $a.Ext;"
          "New-Item -Path $ext -Force|Out-Null;if($a.Mime){$props=Get-ItemProperty -Path $ext "
          "-ErrorAction SilentlyContinue;$owned=$props.ZAPSContentTypeOwner;if(-not "
          "$props.'Content Type' -or $owned -eq $identifier){New-ItemProperty -Path $ext -Name "
          "'Content Type' -Value $a.Mime -PropertyType String -Force|Out-Null;New-ItemProperty "
          "-Path "
          "$ext -Name 'ZAPSContentTypeOwner' -Value $identifier -PropertyType String "
          "-Force|Out-Null}}"
          "$owp=Join-Path $ext 'OpenWithProgids';New-Item -Path $owp -Force|Out-Null;"
          "(Get-Item -LiteralPath $owp).SetValue($a.Prog,[byte[]]@(),[Microsoft.Win32."
          "RegistryValueKind]::None);"
          "$prog=Join-Path $classRoot $a.Prog;RegDefault $prog $a.Desc;New-ItemProperty -Path "
          "$prog "
          "-Name 'ZAPSOwner' -Value $identifier -PropertyType String "
          "-Force|Out-Null;$icon=Join-Path "
          "$prog 'DefaultIcon';RegDefault $icon $assocIcon;$cmd=Join-Path $prog "
          "'shell\\open\\command';"
          "$open='\"'+$assocExe+'\"';if($a.Args){$open+=' '+$a.Args};$open+=' \"%1\"';RegDefault "
          "$cmd $open}}\n";
    ps << "function UnregisterAssoc(){foreach($a in $assocs){$ext=Join-Path $classRoot $a.Ext;"
          "$owp=Join-Path $ext 'OpenWithProgids';Remove-ItemProperty -Path $owp -Name $a.Prog "
          "-ErrorAction SilentlyContinue;$props=Get-ItemProperty -Path $ext -ErrorAction "
          "SilentlyContinue;if($props.ZAPSContentTypeOwner -eq $identifier){Remove-ItemProperty "
          "-Path "
          "$ext -Name 'Content Type' -ErrorAction SilentlyContinue;Remove-ItemProperty -Path $ext "
          "-Name 'ZAPSContentTypeOwner' -ErrorAction SilentlyContinue};$prog=Join-Path $classRoot "
          "$a.Prog;$owner=(Get-ItemProperty -Path $prog -Name 'ZAPSOwner' -ErrorAction "
          "SilentlyContinue).ZAPSOwner;if($owner -eq $identifier){Remove-Item -LiteralPath $prog "
          "-Recurse -Force -ErrorAction SilentlyContinue}}}\n";

    ps << "$remove=@(\n";
    if (uninstallDialog) {
        for (const auto &file : layout.uninstallFiles) {
            if (file.root == WindowsInstallRoot::InstallDir)
                continue;
            ps << ",@(" << powershellSingleQuote(powershellRootCode(file.root)) << ","
               << powershellSingleQuote(powershellRelPath(file.relativePath)) << ")\n";
        }
    }
    ps << ")\n";

    ps << "$dirs=@(\n";
    if (uninstallDialog) {
        for (const auto &dir : layout.uninstallDirectories) {
            if (dir.relativePath.empty())
                continue;
            ps << ",@(" << powershellSingleQuote(powershellRootCode(dir.root)) << ","
               << powershellSingleQuote(powershellRelPath(dir.relativePath)) << ")\n";
        }
    }
    ps << ")\n";

    ps << "if($mode -eq 'rollback'){RestoreTransaction;WriteInstallerLog 'INFO' 'transaction "
          "rolled back';exit 0}\n";
    ps << "if($mode -eq 'commit'){CommitTransaction;WriteInstallerLog 'INFO' 'installation "
          "committed';exit 0}\n";
    ps << "if($mode -eq 'uninstall'){\n";
    ps << "if(Test-Path -LiteralPath $txn){RestoreTransaction}\n";
    ps << "ConfirmDialog ($display+' Uninstall') ('This will remove '+$display+' from this "
          "computer.')\n";
    ps << "RemovePath\n";
    ps << "UnregisterAssoc\n";
    ps << "$manifest=Join-Path $install "
       << powershellSingleQuote(powershellRelPath(layout.installedManifestRelativePath)) << "\n";
    ps << "if(Test-Path -LiteralPath $manifest){Get-Content -LiteralPath $manifest|Sort-Object "
          "Length -Descending|ForEach-Object{if($_){Remove-Item -LiteralPath (Join-Path $install "
          "$_) -Force -ErrorAction SilentlyContinue}}}\n";
    ps << "foreach($f in $remove){RemoveOne $f[0] $f[1]}\n";
    ps << "foreach($d in $dirs){Remove-Item -LiteralPath (Join-Path (Root $d[0]) $d[1]) -Force "
          "-ErrorAction SilentlyContinue}\n";
    ps << "Remove-Item -LiteralPath $uninstallReg -Recurse -Force -ErrorAction SilentlyContinue\n";
    ps << "Remove-Item -LiteralPath $menu -Force -ErrorAction SilentlyContinue\n";
    ps << "$cleanupExe=Join-Path $PSHOME 'powershell.exe';$selfArg='\"'+$self+'\"';$rootArg='\"'+"
          "$install+'\"';Start-Process -FilePath $cleanupExe -ArgumentList @('-NoProfile',"
          "'-NonInteractive','-ExecutionPolicy','Bypass','-EncodedCommand',$cleanupCommand,'--',"
          "$selfArg,$rootArg) -WindowStyle Hidden|Out-Null\n";
    ps << "BroadcastEnv\n";
    ps << "exit 0\n";
    ps << "}\n";

    if (uninstallDialog) {
        ps << "exit 0\n";
    } else {
        ps << "if($mode -ne 'install' -and $mode -ne 'install-files'){throw 'invalid installer "
              "bootstrap mode'}\n";
        ps << "$intro=if($wizardSummary){$wizardSummary}else{'Review the license and install "
              "scope before continuing.'}\n";
        ps << "$msg=$intro+\"`r`n`r`nInstall scope: \"+$scope+\"`r`nInstall location: "
              "\"+$install+\"`r`n`r`n\"+(TrimDialog $license)\n";
        ps << "if($mode -eq 'install'){ConfirmDialog ($display+' Setup') $msg}\n";
        ps << "$files=@(\n";
        for (const auto &file : layout.installFiles) {
            ps << ",@(" << powershellSingleQuote(powershellRootCode(file.root)) << ","
               << powershellSingleQuote(powershellRelPath(file.relativePath)) << ",[int64]"
               << file.overlayDataOffset << ",[int64]" << file.sizeBytes << ","
               << powershellSingleQuote(file.sha256) << ")\n";
        }
        ps << ")\n";
        ps << "try{\n";
        ps << "BeginTransaction\n";
        ps << "if($mode -eq 'install-files'){WriteInstallerLog 'INFO' 'payload staged and "
              "installed';exit 0}\n";
        ps << "New-Item -Path $uninstallReg -Force|Out-Null\n";
        ps << "$uninst=Join-Path $install 'uninstall.exe'\n";
        ps << "SetS 'DisplayName' $display\n";
        ps << "SetS 'DisplayVersion' $version\n";
        ps << "SetS 'Publisher' $publisher\n";
        ps << "SetS 'InstallLocation' $install\n";
        ps << "SetS 'UninstallString' ('\"'+$uninst+'\"')\n";
        ps << "SetS 'QuietUninstallString' ('\"'+$uninst+'\" /quiet')\n";
        if (!iconRel.empty())
            ps << "SetS 'DisplayIcon' " << powershellPathJoinLiteral("$install", iconRel) << "\n";
        ps << "SetD 'NoModify' 1\n";
        ps << "SetD 'NoRepair' 1\n";
        if (layout.estimatedSizeKb != 0)
            ps << "SetD 'EstimatedSize' " << layout.estimatedSizeKb << "\n";
        if (!layout.installDate.empty())
            ps << "SetS 'InstallDate' (Get-Date -Format 'yyyyMMdd')\n";
        ps << "SetS 'URLInfoAbout' $homepage\n";
        ps << "SetS 'URLUpdateInfo' $homepage\n";
        ps << "SetS 'HelpLink' $homepage\n";
        ps << "SetS 'Comments' $comments\n";
        ps << "SetS 'Contact' $contact\n";
        ps << "New-ItemProperty -Path $uninstallReg -Name 'ZAPSShortcutPaths' -Value "
              "$shortcutOwnership -PropertyType String -Force|Out-Null\n";
        ps << "AddPath " << pathEntryExpr << "\n";
        ps << "RegisterAssoc\n";
        ps << "CommitTransaction\n";
        ps << "BroadcastEnv\n";
        ps << "WriteInstallerLog 'INFO' 'installation complete'\n";
        ps << "exit 0\n";
        ps << "}catch{$failure=$_;$rollbackFailure='';try{RestoreTransaction}catch{"
              "$rollbackFailure=$_.Exception.Message};$message='installation failed: '+"
              "$failure.Exception.Message;if($rollbackFailure){$message+='; installer rollback "
              "failed: '+$rollbackFailure};WriteInstallerLog 'ERROR' $message;try{[Console]::"
              "Error.WriteLine($message)}catch{};exit 1}\n";
    }

    return ps.str();
}

/// @brief Build the compact transaction-only backend used by native x64 setup stubs.
/// @details Native x64 code owns the wizard and Windows metadata. Keeping ARM64-only UI and
///          metadata functions out of this script preserves ample headroom below CreateProcessW's
///          32,767-character command-line limit, including layouts with optional shortcuts.
/// @param layout Package paths, payload ownership, integrity data, and scope.
/// @return UTF-8 PowerShell source for extraction, rollback, commit, and removal.
std::string buildNativeTransactionPowerShellScript(const WindowsPackageLayout &layout) {
    const std::string installDir = installDirNameFor(layout);
    const std::string uninstallKey = uninstallKeyPathFor(layout);
    const std::string hive = layout.perUserInstall ? "HKCU:" : "HKLM:";
    const std::string nativeHive = layout.perUserInstall ? "HKCU" : "HKLM";
    const std::string scope = layout.perUserInstall ? "User" : "Machine";

    std::ostringstream ps;
    ps << "$ErrorActionPreference='Stop'\n";
    ps << "$self=$env:ZANNA_INSTALLER_SELF\n";
    ps << "$mode=$env:ZANNA_INSTALLER_MODE\n";
    ps << "$defaultLog=Join-Path ([IO.Path]::GetTempPath()) "
       << powershellSingleQuote("ZannaInstaller-" + registryIdFor(layout) + ".log") << "\n";
    ps << "$log=if($env:ZANNA_INSTALLER_LOG){[Environment]::ExpandEnvironmentVariables($env:"
          "ZANNA_INSTALLER_LOG)}else{$defaultLog}\n";
    ps << "$progressPath=if($env:ZANNA_INSTALLER_PROGRESS){[Environment]::"
          "ExpandEnvironmentVariables($env:ZANNA_INSTALLER_PROGRESS)}else{$null};"
          "$script:lastProgress=-1;$script:lastStatus=''\n";
    ps << "function WriteInstallerLog([string]$level,[string]$message){try{$line=(Get-Date "
          "-Format 'o')+' ['+$level+'] '+$message+[Environment]::NewLine;[IO.File]::"
          "AppendAllText($log,$line,[Text.Encoding]::UTF8)}catch{}}\n";
    ps << "try{if($mode -eq 'install-files'){[IO.File]::WriteAllText($log,'',[Text.Encoding]::"
          "UTF8)};WriteInstallerLog 'INFO' ('backend mode: '+$mode)}catch{}\n";
    ps << "if($mode -eq 'rollback' -and $env:ZANNA_INSTALLER_NATIVE_STAGE){WriteInstallerLog "
          "'ERROR' ('native setup failed during '+$env:ZANNA_INSTALLER_NATIVE_STAGE)}\n";
    ps << "trap{$failure=$_;WriteInstallerLog 'ERROR' ('setup failed: '+$failure.Exception."
          "Message);try{[Console]::Error.WriteLine('setup failed: '+$failure.Exception.Message)}"
          "catch{};exit 1}\n";
    ps << "if(-not $self -or -not (Test-Path -LiteralPath $self -PathType Leaf)){throw 'installer "
          "bootstrap path was not provided'}\n";
    ps << "$installLeaf=" << powershellSingleQuote(installDir) << "\n";
    ps << "$identifier=" << powershellSingleQuote(registryIdFor(layout)) << "\n";
    ps << "$perUser=" << powershellBool(layout.perUserInstall) << "\n";
    ps << "$baseOffset=[int64]" << layout.overlayFileOffset << "\n";
    ps << "$rootBase=if($perUser){[Environment]::GetFolderPath('LocalApplicationData')}else{["
          "Environment]::GetFolderPath('ProgramFiles')}\n";
    ps << "$install=Join-Path $rootBase $installLeaf\n";
    ps << "$desktop=if($perUser){[Environment]::GetFolderPath('Desktop')}else{[Environment]::"
          "GetFolderPath('CommonDesktopDirectory')}\n";
    ps << "$programs=if($perUser){[Environment]::GetFolderPath('Programs')}else{[Environment]::"
          "GetFolderPath('CommonPrograms')}\n";
    ps << "$menu=Join-Path $programs $installLeaf\n";
    ps << "$scope=" << powershellSingleQuote(scope) << "\n";
    ps << "$uninstallReg=" << powershellSingleQuote(hive + "\\" + uninstallKey) << "\n";
    ps << "$regExe=Join-Path $env:SystemRoot 'System32\\reg.exe'\n";
    ps << "$payloadRel="
       << powershellSingleQuote(powershellRelPath(layout.compressedPayloadRelativePath)) << "\n";
    ps << "$nextManifestRel="
       << powershellSingleQuote(powershellRelPath(layout.compressedPayloadManifestRelativePath))
       << "\n";
    ps << "$installedManifestRel="
       << powershellSingleQuote(powershellRelPath(layout.installedManifestRelativePath)) << "\n";
    ps << "$txn=$install+'.zanna-transaction'\n";
    ps << "$txnMarker=Join-Path $txn 'owner'\n";
    ps << "$txnJournal=Join-Path $txn 'journal'\n";
    ps << "$txnBackup=Join-Path $txn 'backup'\n";
    ps << "function Root($r){if($r -eq 'D'){$desktop}elseif($r -eq 'M'){$menu}else{$install}}\n";
    ps << "function Parent($p){$d=[IO.Path]::GetDirectoryName($p);if($d){[IO.Directory]::"
          "CreateDirectory($d)|Out-Null}}\n";
    ps << "function BroadcastEnv(){try{Add-Type -TypeDefinition 'using System; using "
          "System.Runtime.InteropServices; public static class ZannaEnv { "
          "[DllImport(\"user32.dll\", SetLastError=true, CharSet=CharSet.Auto)] public static "
          "extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, "
          "uint flags, uint timeout, out UIntPtr result); }' -ErrorAction SilentlyContinue|"
          "Out-Null;$r=[UIntPtr]::Zero;[void][ZannaEnv]::SendMessageTimeout([IntPtr]0xffff,0x1a,"
          "[UIntPtr]::Zero,'Environment',2,5000,[ref]$r)}catch{}}\n";
    ps << "$regKeys=@(\n";
    ps << ",@(" << powershellSingleQuote(hive + "\\" + uninstallKey) << ","
       << powershellSingleQuote(nativeHive + "\\" + uninstallKey) << ")\n";
    for (const auto &assoc : layout.fileAssociations) {
        const std::string extensionKey = "Software\\Classes\\" + assoc.extension;
        const std::string progIdKey = "Software\\Classes\\" + assoc.progId;
        ps << ",@(" << powershellSingleQuote(hive + "\\" + extensionKey) << ","
           << powershellSingleQuote(nativeHive + "\\" + extensionKey) << ")\n";
        ps << ",@(" << powershellSingleQuote(hive + "\\" + progIdKey) << ","
           << powershellSingleQuote(nativeHive + "\\" + progIdKey) << ")\n";
    }
    ps << ")\n";
    appendPowerShellComponentMetadata(ps, layout, true);
    appendWindowsInstallTransaction(ps, true);
    ps << "$files=@(\n";
    for (const auto &file : layout.installFiles) {
        ps << ",@(" << powershellSingleQuote(powershellRootCode(file.root)) << ","
           << powershellSingleQuote(powershellRelPath(file.relativePath)) << ",[int64]"
           << file.overlayDataOffset << ",[int64]" << file.sizeBytes << ","
           << powershellSingleQuote(file.sha256) << "," << powershellSingleQuote(file.componentId)
           << ")\n";
    }
    ps << ")\n";
    ps << "if($mode -eq 'uninstall-files'){if(Test-Path -LiteralPath $txn){RestoreTransaction};"
          "$manifest=Join-Path $install $installedManifestRel;$selfFull=[IO.Path]::GetFullPath("
          "$self);foreach($rel in @(LoadManifest $manifest $false)){$dst=AssertSafe 'I' $rel;"
          "if(-not [String]::Equals($dst,$selfFull,[StringComparison]::OrdinalIgnoreCase) -and "
          "(Test-Path -LiteralPath $dst -PathType Leaf)){Remove-Item -LiteralPath $dst -Force "
          "-ErrorAction Stop}};$shortcutProps=Get-ItemProperty -LiteralPath $uninstallReg "
          "-ErrorAction SilentlyContinue;$shortcutText=$shortcutProps.ZAPSShortcutPaths;if("
          "$shortcutText){foreach($line in $shortcutText -split \"`n\"){$line=$line.TrimEnd("
          "\"`r\");if(-not $line){continue};$p=$line -split '\\|',2;if($p.Length -ne 2 -or "
          "($p[0] -ne 'D' -and $p[0] -ne 'M')){throw 'invalid installed shortcut ownership "
          "record: '+$line};$dst=AssertSafe $p[0] $p[1];if(Test-Path -LiteralPath $dst "
          "-PathType Leaf){Remove-Item -LiteralPath $dst -Force -ErrorAction Stop}}};"
          "WriteInstallerLog 'INFO' 'selected payload files and shortcuts removed';exit 0}\n";
    ps << "if($mode -eq 'rollback'){ReportProgress 3 'Rolling back incomplete changes...';"
          "RestoreTransaction;WriteInstallerLog 'INFO' 'transaction "
          "rolled back';exit 0}\n";
    ps << "if($mode -eq 'commit'){ReportProgress 99 'Finalizing installation...';"
          "CommitTransaction;ReportProgress 100 'Installation complete.';WriteInstallerLog "
          "'INFO' 'installation "
          "committed';exit 0}\n";
    ps << "if($mode -ne 'install-files'){throw 'invalid native installer backend mode'}\n";
    ps << "try{BeginTransaction;WriteInstallerLog 'INFO' 'payload staged and installed';exit "
          "0}catch{$failure=$_;$rollbackFailure='';try{RestoreTransaction}catch{"
          "$rollbackFailure=$_.Exception.Message};$message='installation failed: '+"
          "$failure.Exception.Message;if($rollbackFailure){$message+='; installer rollback "
          "failed: '+$rollbackFailure};WriteInstallerLog 'ERROR' $message;try{[Console]::"
          "Error.WriteLine($message)}catch{};exit 1}\n";
    return ps.str();
}

/// @brief Gzip and encode a PowerShell source string for `powershell -EncodedCommand`.
/// @param source UTF-8 PowerShell program to compress.
/// @return Base64-encoded UTF-16LE bootstrap that inflates and executes @p source.
/// @details Compression keeps generated command lines below Windows process limits
///          while the small outer bootstrap uses only inbox .NET APIs.
std::string encodedPowerShellCommand(const std::string &source) {
    const auto compressed =
        gzip(reinterpret_cast<const uint8_t *>(source.data()), source.size(), 9);
    const std::string packed = base64Encode(compressed);
    const std::string bootstrap =
        "$ErrorActionPreference='Stop';$z=[Convert]::FromBase64String('" + packed +
        "');$m=New-Object IO.MemoryStream;$m.Write($z,0,$z.Length);$m.Position=0;$g=New-Object "
        "IO.Compression.GZipStream($m,[IO.Compression.CompressionMode]::Decompress);$r=New-Object "
        "IO.StreamReader($g,[Text.Encoding]::UTF8);$s=$r.ReadToEnd();$r.Dispose();$g.Dispose();"
        "$m.Dispose();&([ScriptBlock]::Create($s))";
    return base64Encode(utf8ToUtf16LEBytes(bootstrap, false));
}

/// @brief Build and encode the complete AArch64 PowerShell backend.
/// @param layout Package metadata to embed in the generated program.
/// @param uninstallDialog Whether the script should execute uninstall behavior.
/// @return `-EncodedCommand` payload containing the compressed backend.
std::string encodedArm64PowerShellCommand(const WindowsPackageLayout &layout,
                                          bool uninstallDialog) {
    return encodedPowerShellCommand(buildArm64PowerShellScript(layout, uninstallDialog));
}

/// @brief Build and encode the native x86-64 transaction backend.
/// @param layout Package metadata to embed in the transaction program.
/// @return `-EncodedCommand` payload containing the compressed backend.
std::string encodedNativeTransactionPowerShellCommand(const WindowsPackageLayout &layout) {
    return encodedPowerShellCommand(buildNativeTransactionPowerShellScript(layout));
}

/// @brief Append a predefined Win32 dialog-control class atom.
/// @param out Dialog-template buffer to extend.
/// @param atom System class atom written after the `0xFFFF` marker.
void appendDialogAtomClass(std::vector<uint8_t> &out, uint16_t atom) {
    appendDlgLE16(out, 0xFFFFu);
    appendDlgLE16(out, atom);
}

/// @brief Append one standard-atom control to a Win32 dialog template.
/// @param out Dialog-template buffer to align and extend.
/// @param style Control style bits.
/// @param exStyle Extended control style bits.
/// @param x Left coordinate in dialog units.
/// @param y Top coordinate in dialog units.
/// @param cx Width in dialog units.
/// @param cy Height in dialog units.
/// @param id Dialog control identifier.
/// @param classAtom Predefined system control-class atom.
/// @param title UTF-8 control caption.
void appendDialogControl(std::vector<uint8_t> &out,
                         uint32_t style,
                         uint32_t exStyle,
                         int16_t x,
                         int16_t y,
                         int16_t cx,
                         int16_t cy,
                         uint16_t id,
                         uint16_t classAtom,
                         const std::string &title) {
    alignDialogDword(out);
    appendDlgLE32(out, style);
    appendDlgLE32(out, exStyle);
    appendDlgLE16(out, static_cast<uint16_t>(x));
    appendDlgLE16(out, static_cast<uint16_t>(y));
    appendDlgLE16(out, static_cast<uint16_t>(cx));
    appendDlgLE16(out, static_cast<uint16_t>(cy));
    appendDlgLE16(out, id);
    appendDialogAtomClass(out, classAtom);
    appendDialogWideString(out, title);
    appendDlgLE16(out, 0);
}

/// @brief Append one named-class control to a Win32 dialog template.
/// @param out Dialog-template buffer to align and extend.
/// @param style Control style bits.
/// @param exStyle Extended control style bits.
/// @param x Left coordinate in dialog units.
/// @param y Top coordinate in dialog units.
/// @param cx Width in dialog units.
/// @param cy Height in dialog units.
/// @param id Dialog control identifier.
/// @param className UTF-8 registered window-class name.
/// @param title UTF-8 control caption.
void appendDialogControl(std::vector<uint8_t> &out,
                         uint32_t style,
                         uint32_t exStyle,
                         int16_t x,
                         int16_t y,
                         int16_t cx,
                         int16_t cy,
                         uint16_t id,
                         const std::string &className,
                         const std::string &title) {
    alignDialogDword(out);
    appendDlgLE32(out, style);
    appendDlgLE32(out, exStyle);
    appendDlgLE16(out, static_cast<uint16_t>(x));
    appendDlgLE16(out, static_cast<uint16_t>(y));
    appendDlgLE16(out, static_cast<uint16_t>(cx));
    appendDlgLE16(out, static_cast<uint16_t>(cy));
    appendDlgLE16(out, id);
    appendDialogWideString(out, className);
    appendDialogWideString(out, title);
    appendDlgLE16(out, 0);
}

/// @brief Build the modal setup or uninstall wizard dialog resource.
/// @param layout UI text, component list, scope, and license metadata.
/// @param uninstallDialog Whether controls should present uninstall confirmation.
/// @return Serialized standard Win32 dialog template bytes.
/// @details Installs the Segoe UI font, branded banner, description/license
///          controls, component checkboxes where applicable, and OK/Cancel actions.
std::vector<uint8_t> buildWizardDialogTemplate(const WindowsPackageLayout &layout,
                                               bool uninstallDialog) {
    constexpr uint16_t kButtonClass = 0x0080;
    constexpr uint16_t kEditClass = 0x0081;
    constexpr uint16_t kStaticClass = 0x0082;
    constexpr uint32_t kWsChildVisible = 0x50000000u;
    constexpr uint32_t kWsTabStop = 0x00010000u;
    constexpr uint32_t kDsModalFrame = 0x00000080u;
    constexpr uint32_t kDsSetFont = 0x00000040u;
    constexpr uint32_t kDsCenter = 0x00000800u;
    constexpr uint32_t kWsPopup = 0x80000000u;
    constexpr uint32_t kWsCaption = 0x00C00000u;
    constexpr uint32_t kWsSysMenu = 0x00080000u;
    constexpr uint32_t kWsBorder = 0x00800000u;
    constexpr uint32_t kWsVScroll = 0x00200000u;
    constexpr uint32_t kSsLeft = 0x00000000u;
    constexpr uint32_t kSsCenter = 0x00000001u;
    constexpr uint32_t kSsCenterImage = 0x00000200u;
    constexpr uint32_t kSsOwnerDraw = 0x0000000Du;
    constexpr uint32_t kEsMultiline = 0x00000004u;
    constexpr uint32_t kEsAutoVScroll = 0x00000040u;
    constexpr uint32_t kEsReadOnly = 0x00000800u;
    constexpr uint32_t kBsPushButton = 0x00000000u;
    constexpr uint32_t kBsDefPushButton = 0x00000001u;
    constexpr uint32_t kBsAutoCheckBox = 0x00000003u;

    const bool branded = !uninstallDialog && !layout.wizardImageRgba.empty();
    const size_t componentCount = uninstallDialog ? 0u : layout.optionalComponents.size();
    const std::string title = layout.displayName + (uninstallDialog ? " Uninstall" : " Setup");
    const std::string intro =
        !layout.wizardSummary.empty()
            ? layout.wizardSummary
            : (uninstallDialog ? "Review the removal summary, confirm, then choose Uninstall."
                               : "Review the license and destination, then choose Install.");
    const std::string scopeSummary =
        std::string("Scope: ") + (layout.perUserInstall ? "Current user" : "All users") +
        "    Destination: " + (layout.perUserInstall ? "%LocalAppData%\\" : "%ProgramFiles%\\") +
        installDirNameFor(layout);
    std::string body = uninstallDialog
                           ? ("This will remove " + layout.displayName + " from this computer.")
                           : layout.licenseText;
    if (body.empty())
        body = "GPL-3.0-only";
    body = normalizeDialogText(body);

    constexpr int16_t kWidth = 500;
    constexpr int16_t kBannerHeight = 88;
    constexpr int16_t kComponentStart = 226;
    constexpr int16_t kComponentStride = 24;
    const int16_t scopeY = static_cast<int16_t>(
        componentCount == 0
            ? 210
            : kComponentStart + static_cast<int16_t>(componentCount) * kComponentStride + 5);
    const int16_t buttonY = static_cast<int16_t>(scopeY + 23);
    const int16_t dialogHeight = static_cast<int16_t>(buttonY + 32);
    const uint16_t controlCount =
        static_cast<uint16_t>(8u + (componentCount == 0 ? 0u : 1u + componentCount * 2u));

    std::vector<uint8_t> out;
    appendDlgLE32(out, kDsModalFrame | kDsSetFont | kDsCenter | kWsPopup | kWsCaption | kWsSysMenu);
    appendDlgLE32(out, 0);
    appendDlgLE16(out, controlCount);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, kWidth);
    appendDlgLE16(out, dialogHeight);
    appendDlgLE16(out, 0);
    appendDlgLE16(out, 0);
    appendDialogWideString(out, title);
    appendDlgLE16(out, 10);
    appendDialogWideString(out, "Segoe UI");

    appendDialogControl(out,
                        kWsChildVisible | (branded ? kSsOwnerDraw : kSsCenter | kSsCenterImage),
                        0,
                        0,
                        0,
                        kWidth,
                        kBannerHeight,
                        kDlgIdBanner,
                        kStaticClass,
                        branded ? "" : title);
    appendDialogControl(
        out, kWsChildVisible | kSsLeft, 0, 20, 96, 460, 18, 2002, kStaticClass, intro);
    appendDialogControl(out,
                        kWsChildVisible | kSsLeft,
                        0,
                        20,
                        118,
                        130,
                        10,
                        2004,
                        kStaticClass,
                        uninstallDialog ? "Removal summary" : "License agreement");
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kWsBorder | kWsVScroll | kEsMultiline |
                            kEsAutoVScroll | kEsReadOnly,
                        0,
                        20,
                        130,
                        460,
                        54,
                        kDlgIdLicense,
                        kEditClass,
                        body);
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kBsAutoCheckBox,
                        0,
                        20,
                        190,
                        300,
                        12,
                        kDlgIdAccept,
                        kButtonClass,
                        uninstallDialog ? "I understand and want to continue"
                                        : "I accept the license agreement");

    if (componentCount != 0) {
        appendDialogControl(out,
                            kWsChildVisible | kSsLeft,
                            0,
                            20,
                            210,
                            460,
                            12,
                            2005,
                            kStaticClass,
                            "Developer components (all selected by default)");
        for (size_t i = 0; i < componentCount; ++i) {
            const auto &component = layout.optionalComponents[i];
            const int16_t y = static_cast<int16_t>(kComponentStart + i * kComponentStride);
            appendDialogControl(out,
                                kWsChildVisible | kWsTabStop | kBsAutoCheckBox,
                                0,
                                20,
                                y,
                                450,
                                12,
                                static_cast<uint16_t>(kDlgIdComponentBase + i),
                                kButtonClass,
                                component.label);
            appendDialogControl(out,
                                kWsChildVisible | kSsLeft,
                                0,
                                35,
                                static_cast<int16_t>(y + 13),
                                435,
                                9,
                                static_cast<uint16_t>(2100 + i),
                                kStaticClass,
                                component.description);
        }
    }
    appendDialogControl(
        out, kWsChildVisible | kSsLeft, 0, 20, scopeY, 460, 18, 2003, kStaticClass, scopeSummary);
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kBsDefPushButton,
                        0,
                        340,
                        buttonY,
                        66,
                        20,
                        kDlgIdOk,
                        kButtonClass,
                        uninstallDialog ? "Uninstall" : "Install");
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kBsPushButton,
                        0,
                        414,
                        buttonY,
                        66,
                        20,
                        kDlgIdCancel,
                        kButtonClass,
                        "Cancel");

    alignDialogDword(out);
    return out;
}

/// @brief Build the compact dark-themed completion/error dialog used after interactive setup.
/// @param title UTF-8 dialog caption and optional unbranded banner text.
/// @param message UTF-8 multi-line result description.
/// @param branded Whether the owner-drawn package artwork replaces text branding.
/// @return Serialized standard Win32 dialog template bytes.
std::vector<uint8_t> buildResultDialogTemplate(const std::string &title,
                                               const std::string &message,
                                               bool branded) {
    constexpr uint16_t kButtonClass = 0x0080;
    constexpr uint16_t kEditClass = 0x0081;
    constexpr uint16_t kStaticClass = 0x0082;
    constexpr uint32_t kWsChildVisible = 0x50000000u;
    constexpr uint32_t kWsTabStop = 0x00010000u;
    constexpr uint32_t kDsModalFrame = 0x00000080u;
    constexpr uint32_t kDsSetFont = 0x00000040u;
    constexpr uint32_t kDsCenter = 0x00000800u;
    constexpr uint32_t kWsPopup = 0x80000000u;
    constexpr uint32_t kWsCaption = 0x00C00000u;
    constexpr uint32_t kWsSysMenu = 0x00080000u;
    constexpr uint32_t kWsBorder = 0x00800000u;
    constexpr uint32_t kSsCenter = 0x00000001u;
    constexpr uint32_t kSsCenterImage = 0x00000200u;
    constexpr uint32_t kSsOwnerDraw = 0x0000000Du;
    constexpr uint32_t kEsMultiline = 0x00000004u;
    constexpr uint32_t kEsReadOnly = 0x00000800u;
    constexpr uint32_t kBsDefPushButton = 0x00000001u;

    std::vector<uint8_t> out;
    appendDlgLE32(out, kDsModalFrame | kDsSetFont | kDsCenter | kWsPopup | kWsCaption | kWsSysMenu);
    appendDlgLE32(out, 0);
    appendDlgLE16(out, 3);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, 500);
    appendDlgLE16(out, 222);
    appendDlgLE16(out, 0);
    appendDlgLE16(out, 0);
    appendDialogWideString(out, title);
    appendDlgLE16(out, 10);
    appendDialogWideString(out, "Segoe UI");

    appendDialogControl(out,
                        kWsChildVisible | (branded ? kSsOwnerDraw : kSsCenter | kSsCenterImage),
                        0,
                        0,
                        0,
                        500,
                        88,
                        kDlgIdBanner,
                        kStaticClass,
                        branded ? "" : title);
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kWsBorder | kEsMultiline | kEsReadOnly,
                        0,
                        20,
                        103,
                        460,
                        72,
                        kDlgIdLicense,
                        kEditClass,
                        normalizeDialogText(message));
    appendDialogControl(out,
                        kWsChildVisible | kWsTabStop | kBsDefPushButton,
                        0,
                        414,
                        192,
                        66,
                        20,
                        kDlgIdOk,
                        kButtonClass,
                        "OK");
    alignDialogDword(out);
    return out;
}

/// @brief Build the modeless branded progress surface kept visible while setup is working.
/// @param layout Package display name and optional wizard artwork.
/// @return Serialized dialog template containing banner, status, and progress controls.
std::vector<uint8_t> buildProgressDialogTemplate(const WindowsPackageLayout &layout) {
    constexpr uint16_t kStaticClass = 0x0082;
    constexpr uint32_t kWsChildVisible = 0x50000000u;
    constexpr uint32_t kDsModalFrame = 0x00000080u;
    constexpr uint32_t kDsSetFont = 0x00000040u;
    constexpr uint32_t kDsCenter = 0x00000800u;
    constexpr uint32_t kWsPopup = 0x80000000u;
    constexpr uint32_t kWsCaption = 0x00C00000u;
    constexpr uint32_t kWsSysMenu = 0x00080000u;
    constexpr uint32_t kSsLeft = 0x00000000u;
    constexpr uint32_t kSsCenter = 0x00000001u;
    constexpr uint32_t kSsCenterImage = 0x00000200u;
    constexpr uint32_t kSsOwnerDraw = 0x0000000Du;
    constexpr uint32_t kPbsSmooth = 0x00000001u;
    const bool branded = !layout.wizardImageRgba.empty();
    const std::string title = layout.displayName + " Setup";

    std::vector<uint8_t> out;
    appendDlgLE32(out, kDsModalFrame | kDsSetFont | kDsCenter | kWsPopup | kWsCaption | kWsSysMenu);
    appendDlgLE32(out, 0);
    appendDlgLE16(out, 4);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, 10);
    appendDlgLE16(out, 500);
    appendDlgLE16(out, 210);
    appendDlgLE16(out, 0);
    appendDlgLE16(out, 0);
    appendDialogWideString(out, title);
    appendDlgLE16(out, 10);
    appendDialogWideString(out, "Segoe UI");

    appendDialogControl(out,
                        kWsChildVisible | (branded ? kSsOwnerDraw : kSsCenter | kSsCenterImage),
                        0,
                        0,
                        0,
                        500,
                        88,
                        kDlgIdBanner,
                        kStaticClass,
                        branded ? "" : title);
    appendDialogControl(out,
                        kWsChildVisible | kSsLeft,
                        0,
                        20,
                        110,
                        460,
                        18,
                        kDlgIdProgressStatus,
                        kStaticClass,
                        "Preparing the Zanna developer toolchain...");
    appendDialogControl(out,
                        kWsChildVisible | kPbsSmooth,
                        0,
                        20,
                        136,
                        460,
                        18,
                        kDlgIdProgressBar,
                        "msctls_progress32",
                        "");
    appendDialogControl(out,
                        kWsChildVisible | kSsLeft,
                        0,
                        20,
                        167,
                        460,
                        20,
                        2003,
                        kStaticClass,
                        "Setup stays visible while files are verified and installed. Please wait.");
    alignDialogDword(out);
    return out;
}

/// @brief Win32 DIB header and converted pixel payload for wizard artwork.
struct WizardDibData {
    std::vector<uint8_t> infoHeader; ///< Serialized 40-byte BITMAPINFOHEADER.
    std::vector<uint8_t> bgraPixels; ///< Top-down pixels reordered from RGBA to BGRA.
};

/// @brief Convert the package's canonical top-down RGBA banner to a Win32 32-bit DIB.
/// @param layout Wizard dimensions and RGBA byte vector.
/// @return Empty buffers when no artwork is supplied, otherwise header and BGRA pixels.
/// @pre Nonempty artwork contains exactly width × height × four bytes.
WizardDibData buildWizardDib(const WindowsPackageLayout &layout) {
    WizardDibData out;
    if (layout.wizardImageRgba.empty())
        return out;

    appendDlgLE32(out.infoHeader, 40);
    appendDlgLE32(out.infoHeader, layout.wizardImageWidth);
    appendDlgLE32(out.infoHeader, 0u - layout.wizardImageHeight); // Negative means top-down.
    appendDlgLE16(out.infoHeader, 1);
    appendDlgLE16(out.infoHeader, 32);
    appendDlgLE32(out.infoHeader, 0); // BI_RGB
    appendDlgLE32(out.infoHeader, static_cast<uint32_t>(layout.wizardImageRgba.size()));
    appendDlgLE32(out.infoHeader, 0);
    appendDlgLE32(out.infoHeader, 0);
    appendDlgLE32(out.infoHeader, 0);
    appendDlgLE32(out.infoHeader, 0);

    out.bgraPixels.resize(layout.wizardImageRgba.size());
    for (size_t i = 0; i < layout.wizardImageRgba.size(); i += 4) {
        out.bgraPixels[i] = layout.wizardImageRgba[i + 2];
        out.bgraPixels[i + 1] = layout.wizardImageRgba[i + 1];
        out.bgraPixels[i + 2] = layout.wizardImageRgba[i];
        out.bgraPixels[i + 3] = layout.wizardImageRgba[i + 3];
    }
    return out;
}

/// @brief Resolve the payload architecture to the bootstrap PE machine type.
/// @param payloadArch Requested package architecture; empty means x64.
/// @return Canonical `"x64"` or `"arm64"` bootstrap architecture.
/// @throws std::runtime_error For any unsupported architecture string.
std::string resolveBootstrapArch(const std::string &payloadArch) {
    if (payloadArch.empty() || payloadArch == "x64")
        return "x64";
    if (payloadArch == "arm64")
        return "arm64";
    throw std::runtime_error("unsupported Windows package architecture '" + payloadArch + "'");
}

/// @brief Compute the byte offset within the .rdata section where the IAT array begins.
/// Layout: IDT (20 × (N+1) bytes) + ILTs + hint/name table + DLL name strings, rounded
/// up to 8-byte alignment. This offset is required to patch RIP-relative IAT call
/// targets in the already-emitted machine code.
/// @param imports Ordered PE import descriptors and function-name lists.
/// @return Eight-byte-aligned offset from `.rdata` start to the IAT array.
uint32_t computeIATOffset(const std::vector<PEImport> &imports) {
    if (imports.empty())
        return 0;

    uint32_t idtSize = static_cast<uint32_t>((imports.size() + 1) * 20);
    uint32_t iltSize = 0;
    for (const auto &imp : imports)
        iltSize += static_cast<uint32_t>((imp.functions.size() + 1) * 8);
    uint32_t hintNameSize = 0;
    for (const auto &imp : imports) {
        for (const auto &fn : imp.functions) {
            const uint32_t entryLen = static_cast<uint32_t>(2 + fn.size() + 1);
            hintNameSize += (entryLen + 1) & ~1u;
        }
    }
    uint32_t dllNameSize = 0;
    for (const auto &imp : imports)
        dllNameSize += static_cast<uint32_t>(imp.dllName.size() + 1);

    uint32_t iatOff = idtSize + iltSize + hintNameSize + dllNameSize;
    return alignUp(iatOff, 8);
}

/// @brief Finalize section RVAs, patch all IAT call offsets, and populate stub.textSection.
/// Must be called exactly once after all emit* functions have run. Places .text at kTextRVA
/// and .rdata immediately after (aligned to kSectionAlignment); computes the IAT base RVA
/// and stubData RVA offset, then calls gen.finishText() to apply all fixups.
/// @param stub Result receiving data offset and finalized x86-64 text bytes.
/// @param gen Completed x86-64 instruction/data-reference emitter.
void finalizeStubRVAs(StubResult &stub, InstallerStubGen &gen) {
    const uint32_t textRVA = kTextRVA;
    const uint32_t rdataRVA =
        kRdataBaseRVA +
        (alignUp(static_cast<uint32_t>(gen.codeSize()), kSectionAlignment) - kSectionAlignment);

    const uint32_t iatOff = computeIATOffset(stub.imports);
    const uint32_t iatBaseRVA = rdataRVA + iatOff;

    uint32_t iatSize = 0;
    for (const auto &imp : stub.imports)
        iatSize += static_cast<uint32_t>((imp.functions.size() + 1) * 8);

    const uint32_t dataBaseRVA = iatBaseRVA + iatSize;
    stub.stubDataRVAOffset = dataBaseRVA - rdataRVA;
    stub.textSection = gen.finishText(textRVA, iatBaseRVA, stub.imports, dataBaseRVA);
}

/// @brief Finalize AArch64 section RVAs, IAT calls, and result text bytes.
/// @param stub Result receiving data offset and finalized AArch64 text bytes.
/// @param gen Completed AArch64 instruction/data-reference emitter.
/// @details Mirrors the x86-64 overload while applying the AArch64 emitter's fixups.
void finalizeStubRVAs(StubResult &stub, InstallerStubGenA64 &gen) {
    const uint32_t textRVA = kTextRVA;
    const uint32_t rdataRVA =
        kRdataBaseRVA +
        (alignUp(static_cast<uint32_t>(gen.codeSize()), kSectionAlignment) - kSectionAlignment);

    const uint32_t iatOff = computeIATOffset(stub.imports);
    const uint32_t iatBaseRVA = rdataRVA + iatOff;

    uint32_t iatSize = 0;
    for (const auto &imp : stub.imports)
        iatSize += static_cast<uint32_t>((imp.functions.size() + 1) * 8);

    const uint32_t dataBaseRVA = iatBaseRVA + iatSize;
    stub.stubDataRVAOffset = dataBaseRVA - rdataRVA;
    stub.textSection = gen.finishText(textRVA, iatBaseRVA, stub.imports, dataBaseRVA);
}

/// @brief Emit code to zero an 8-byte local variable at [RBP+off] via XOR-zero + MOV.
/// @param gen x86-64 instruction emitter.
/// @param off Signed frame offset of the qword local.
void zeroLocalQword(InstallerStubGen &gen, int32_t off) {
    gen.xorRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.movMemReg(X64Reg::RBP, off, X64Reg::RAX);
}

/// @brief Zero a small fixed local range using 8-byte stores.
/// @param gen x86-64 instruction emitter.
/// @param off Signed frame offset of the first qword.
/// @param bytes Byte count to clear in eight-byte increments.
/// @pre @p bytes is a multiple of eight and the complete range is frame-local.
void zeroLocalRange(InstallerStubGen &gen, int32_t off, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 8)
        zeroLocalQword(gen, off + static_cast<int32_t>(i));
}

/// @brief Emit code to store a 64-bit immediate into [RSP+off].
/// Used to pass the 5th+ arguments in a Windows x64 call (beyond the four shadow-space registers).
/// @param gen x86-64 instruction emitter.
/// @param off Stack-pointer-relative outgoing argument offset.
/// @param imm Immediate value to store.
void storeStackImm64(InstallerStubGen &gen, int32_t off, uint64_t imm) {
    gen.movRegImm64(X64Reg::RAX, imm);
    gen.movMemReg(X64Reg::RSP, off, X64Reg::RAX);
}

/// @brief Emit code to compute &[RBP+localOff] into RAX and store it at [RSP+stackOff].
/// Used to pass a pointer to a stack-resident buffer as a 5th+ Win32 argument.
/// @param gen x86-64 instruction emitter.
/// @param stackOff Outgoing stack argument offset.
/// @param localOff Frame offset whose address should be passed.
void storeStackPtrToLocal(InstallerStubGen &gen, int32_t stackOff, int32_t localOff) {
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, localOff);
    gen.movMemReg(X64Reg::RSP, stackOff, X64Reg::RAX);
}

/// @brief Return the byte size of `text` as a UTF-16LE string including the null terminator.
/// @param text UTF-8 text whose transcoded storage size is required.
/// @return UTF-16LE byte count including one terminating code unit.
/// @throws std::runtime_error If the result exceeds the DWORD accepted by RegSetValueExW.
uint32_t wideBytesFor(const std::string &text) {
    const size_t bytes = (utf16CodeUnitCountFromUtf8(text) + 1) * 2;
    if (bytes > UINT32_MAX)
        throw std::runtime_error("Windows installer string is too large");
    return static_cast<uint32_t>(bytes);
}

/// @brief Return the install directory name, using `displayName` as fallback when `installDirName`
/// is empty.
/// @param layout Package naming metadata.
/// @return Explicit install directory name, or display name when absent.
std::string installDirNameFor(const WindowsPackageLayout &layout) {
    return layout.installDirName.empty() ? layout.displayName : layout.installDirName;
}

/// @brief Return the registry hive used for package-owned metadata.
/// @param layout Installation scope selector.
/// @return Predefined HKCU handle for per-user installs, otherwise HKLM.
uint64_t registryRootFor(const WindowsPackageLayout &layout) {
    return layout.perUserInstall ? kHkeyCurrentUser : kHkeyLocalMachine;
}

using KnownFolderGuid = std::array<uint8_t, 16>;

constexpr KnownFolderGuid kFolderIdProgramFiles = {
    0xb6, 0x63, 0x5e, 0x90, 0xbf, 0xc1, 0x4e, 0x49, 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a};
constexpr KnownFolderGuid kFolderIdLocalAppData = {
    0x85, 0x27, 0xb3, 0xf1, 0xba, 0x6f, 0xcf, 0x4f, 0x9d, 0x55, 0x7b, 0x8e, 0x7f, 0x15, 0x70, 0x91};
constexpr KnownFolderGuid kFolderIdDesktop = {
    0x3a, 0xcc, 0xbf, 0xb4, 0x2c, 0xdb, 0x4c, 0x42, 0xb0, 0x29, 0x7f, 0xe9, 0x9a, 0x87, 0xc6, 0x41};
constexpr KnownFolderGuid kFolderIdPublicDesktop = {
    0x0d, 0x34, 0xaa, 0xc4, 0x0f, 0xf2, 0x63, 0x48, 0xaf, 0xef, 0xf8, 0x7e, 0xf2, 0xe6, 0xba, 0x25};
constexpr KnownFolderGuid kFolderIdPrograms = {
    0x77, 0x5d, 0x7f, 0xa7, 0x2b, 0x2e, 0xc3, 0x44, 0xa6, 0xa2, 0xab, 0xa6, 0x01, 0x05, 0x4a, 0x51};
constexpr KnownFolderGuid kFolderIdCommonPrograms = {
    0x4e, 0xd4, 0x39, 0x01, 0xfe, 0x6a, 0xf2, 0x49, 0x86, 0x90, 0x3d, 0xaf, 0xca, 0xe6, 0xff, 0xb8};

/// @brief KNOWNFOLDERID for the install root: LocalAppData (per-user) or ProgramFiles.
/// @param layout Installation scope selector.
/// @return Reference to static LocalAppData or ProgramFiles GUID bytes.
const KnownFolderGuid &installRootFolderIdFor(const WindowsPackageLayout &layout) {
    return layout.perUserInstall ? kFolderIdLocalAppData : kFolderIdProgramFiles;
}

/// @brief KNOWNFOLDERID for the desktop: the user's Desktop (per-user) or PublicDesktop.
/// @param layout Installation scope selector.
/// @return Reference to static user or public Desktop GUID bytes.
const KnownFolderGuid &desktopFolderIdFor(const WindowsPackageLayout &layout) {
    return layout.perUserInstall ? kFolderIdDesktop : kFolderIdPublicDesktop;
}

/// @brief KNOWNFOLDERID for Start Menu Programs: per-user Programs or CommonPrograms.
/// @param layout Installation scope selector.
/// @return Reference to static user or common Programs GUID bytes.
const KnownFolderGuid &programsFolderIdFor(const WindowsPackageLayout &layout) {
    return layout.perUserInstall ? kFolderIdPrograms : kFolderIdCommonPrograms;
}

/// @brief Return the registry key used for PATH changes.
/// @param layout Installation scope selector.
/// @return Scope-relative environment registry key path.
std::string environmentKeyPathFor(const WindowsPackageLayout &layout) {
    return layout.perUserInstall
               ? "Environment"
               : "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
}

/// @brief Return the registry key identifier for this package.
/// Preference order: explicit `identifier`, normalized `executableName`, normalized `displayName`.
/// @param layout Package identity and executable/display fallbacks.
/// @return Registry-safe stable identifier.
std::string registryIdFor(const WindowsPackageLayout &layout) {
    if (!layout.identifier.empty())
        return layout.identifier;
    if (!layout.executableName.empty())
        return normalizeExecName(layout.executableName);
    return normalizeExecName(layout.displayName);
}

/// @brief Convert all forward slashes in `text` to backslashes for use as a Windows path component.
/// @param text Path fragment copied for in-place conversion.
/// @return Windows-separator path fragment.
std::string windowsPathFragment(std::string text) {
    for (char &ch : text) {
        if (ch == '/')
            ch = '\\';
    }
    return text;
}

/// @brief Build the hive-relative Add/Remove Programs key for this package.
/// @param layout Package identity used as the final subkey.
/// @return Path beneath the selected HKCU or HKLM hive.
std::string uninstallKeyPathFor(const WindowsPackageLayout &layout) {
    return "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + registryIdFor(layout);
}

/// @brief Return true if the Desktop path buffer must be resolved at runtime.
/// True when a desktop shortcut is requested or any install/uninstall file entry
/// targets WindowsInstallRoot::DesktopDir.
/// @param layout Package shortcut and file destinations to inspect.
/// @return True when generated code needs the Desktop known-folder buffer.
bool needsDesktopPath(const WindowsPackageLayout &layout) {
    if (layout.createDesktopShortcut)
        return true;
    /// @brief Test whether an installation file targets the Windows Desktop.
    /// @param entry File entry to inspect.
    /// @return `true` when the entry is rooted at the Desktop directory.
    return std::any_of(layout.installFiles.begin(),
                       layout.installFiles.end(),
                       [](const WindowsPackageFileEntry &entry) {
                           return entry.root == WindowsInstallRoot::DesktopDir;
                       }) ||
           /// @brief Test whether an uninstallation file targets the Windows Desktop.
           /// @param entry File entry to inspect.
           /// @return `true` when the entry is rooted at the Desktop directory.
           std::any_of(layout.uninstallFiles.begin(),
                       layout.uninstallFiles.end(),
                       [](const WindowsPackageFileEntry &entry) {
                           return entry.root == WindowsInstallRoot::DesktopDir;
                       });
}

/// @brief Return true if the Start Menu path buffer must be resolved at runtime.
/// True when a Start Menu shortcut is requested or any install/uninstall file or
/// directory entry targets WindowsInstallRoot::StartMenuDir.
/// @param layout Package shortcut, file, and directory destinations to inspect.
/// @return True when generated code needs the Programs known-folder buffer.
bool needsMenuPath(const WindowsPackageLayout &layout) {
    if (layout.createStartMenuShortcut)
        return true;
    /// @brief Test whether an installation file targets the Windows Start Menu.
    /// @param entry File entry to inspect.
    /// @return `true` when the entry is rooted at the Start Menu directory.
    return std::any_of(layout.installFiles.begin(),
                       layout.installFiles.end(),
                       [](const WindowsPackageFileEntry &entry) {
                           return entry.root == WindowsInstallRoot::StartMenuDir;
                       }) ||
           /// @brief Test whether an uninstallation file targets the Windows Start Menu.
           /// @param entry File entry to inspect.
           /// @return `true` when the entry is rooted at the Start Menu directory.
           std::any_of(layout.uninstallFiles.begin(),
                       layout.uninstallFiles.end(),
                       [](const WindowsPackageFileEntry &entry) {
                           return entry.root == WindowsInstallRoot::StartMenuDir;
                       }) ||
           /// @brief Test whether an installation directory targets the Windows Start Menu.
           /// @param entry Directory entry to inspect.
           /// @return `true` when the entry is rooted at the Start Menu directory.
           std::any_of(layout.installDirectories.begin(),
                       layout.installDirectories.end(),
                       [](const WindowsPackageDirEntry &entry) {
                           return entry.root == WindowsInstallRoot::StartMenuDir;
                       }) ||
           /// @brief Test whether an uninstallation directory targets the Windows Start Menu.
           /// @param entry Directory entry to inspect.
           /// @return `true` when the entry is rooted at the Start Menu directory.
           std::any_of(layout.uninstallDirectories.begin(),
                       layout.uninstallDirectories.end(),
                       [](const WindowsPackageDirEntry &entry) {
                           return entry.root == WindowsInstallRoot::StartMenuDir;
                       });
}

/// @brief Map a WindowsInstallRoot anchor to its corresponding stack-frame local buffer offset.
/// @param root Logical install destination.
/// @return Frame offset of the install, Desktop, or Start Menu path buffer.
int32_t rootBufferOffset(WindowsInstallRoot root) {
    switch (root) {
        case WindowsInstallRoot::DesktopDir:
            return kDesktopPathOff;
        case WindowsInstallRoot::StartMenuDir:
            return kMenuPathOff;
        case WindowsInstallRoot::InstallDir:
        default:
            return kInstallPathOff;
    }
}

/// @brief Emit a null-guarded CloseHandle for the HANDLE at [RBP+handleOff], then zero the slot.
/// Safe to call when the slot is already zero; the null check skips the call.
/// @param gen x86-64 instruction emitter.
/// @param handleOff Frame offset of the local HANDLE.
/// @param closeSlot Flat IAT slot for `CloseHandle`.
void emitCloseLocalHandleIfSet(InstallerStubGen &gen, int32_t handleOff, uint32_t closeSlot) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, handleOff);
    gen.testRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.jz(lblSkip);
    gen.callIATSlot(closeSlot);
    zeroLocalQword(gen, handleOff);
    gen.bindLabel(lblSkip);
}

/// @brief Emit a null-guarded LocalFree for the heap pointer at [RBP+ptrOff], then zero the slot.
/// @param gen x86-64 instruction emitter.
/// @param ptrOff Frame offset of the local allocation pointer.
/// @param freeSlot Flat IAT slot for `LocalFree`.
void emitLocalFreeIfSet(InstallerStubGen &gen, int32_t ptrOff, uint32_t freeSlot) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, ptrOff);
    gen.testRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.jz(lblSkip);
    gen.callIATSlot(freeSlot);
    zeroLocalQword(gen, ptrOff);
    gen.bindLabel(lblSkip);
}

/// @brief Emit a null-guarded RegCloseKey for the HKEY at [RBP+keyOff], then zero the slot.
/// @param gen x86-64 instruction emitter.
/// @param keyOff Frame offset of the local HKEY.
/// @param closeSlot Flat IAT slot for `RegCloseKey`.
void emitRegCloseIfSet(InstallerStubGen &gen, int32_t keyOff, uint32_t closeSlot) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, keyOff);
    gen.testRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.jz(lblSkip);
    gen.callIATSlot(closeSlot);
    zeroLocalQword(gen, keyOff);
    gen.bindLabel(lblSkip);
}

/// @brief Emit lstrcatW of a RIP-relative embedded wide string onto [RBP+destOff].
/// Checks combined length against kMaxPathChars before concatenating; jumps to errorLabel on
/// overflow.
/// @param gen x86-64 instruction emitter.
/// @param destOff Frame offset of the mutable destination buffer.
/// @param srcDataOff Stub-data offset of the NUL-terminated source string.
/// @param catSlot Flat IAT slot for `lstrcatW`.
/// @param strlenSlot Flat IAT slot for `lstrlenW`.
/// @param errorLabel Branch target for a combined-length overflow.
void emitCheckedCatEmbedded(InstallerStubGen &gen,
                            int32_t destOff,
                            uint32_t srcDataOff,
                            uint32_t catSlot,
                            uint32_t strlenSlot,
                            uint32_t errorLabel) {
    gen.leaRipData(X64Reg::RAX, srcDataOff);
    gen.movMemReg(X64Reg::RBP, kBytesWrittenOff, X64Reg::RAX);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.callIATSlot(strlenSlot);
    gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(strlenSlot);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesReadOff);
    gen.addRegReg(X64Reg::RAX, X64Reg::RDX);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(catSlot);
}

/// @brief Emit lstrcatW of the stack buffer at [RBP+srcOff] onto [RBP+destOff].
/// Length-checks against kMaxPathChars before concatenating; jumps to errorLabel on overflow.
/// @param gen x86-64 instruction emitter.
/// @param destOff Frame offset of the mutable destination buffer.
/// @param srcOff Frame offset of the NUL-terminated source buffer.
/// @param catSlot Flat IAT slot for `lstrcatW`.
/// @param strlenSlot Flat IAT slot for `lstrlenW`.
/// @param errorLabel Branch target for a combined-length overflow.
void emitCheckedCatStack(InstallerStubGen &gen,
                         int32_t destOff,
                         int32_t srcOff,
                         uint32_t catSlot,
                         uint32_t strlenSlot,
                         uint32_t errorLabel) {
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, srcOff);
    gen.movMemReg(X64Reg::RBP, kBytesWrittenOff, X64Reg::RAX);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.callIATSlot(strlenSlot);
    gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(strlenSlot);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesReadOff);
    gen.addRegReg(X64Reg::RAX, X64Reg::RDX);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(catSlot);
}

/// @brief Emit lstrcatW of the wide string in `srcReg` onto [RBP+destOff].
/// Length-checks against kMaxPathChars before concatenating; jumps to errorLabel on overflow.
/// @param gen Instruction emitter.
/// @param destOff Destination frame-buffer offset.
/// @param srcReg Register holding the source string pointer.
/// @param catSlot `lstrcatW` IAT slot.
/// @param strlenSlot `lstrlenW` IAT slot.
/// @param errorLabel Overflow branch target.
void emitCheckedCatReg(InstallerStubGen &gen,
                       int32_t destOff,
                       X64Reg srcReg,
                       uint32_t catSlot,
                       uint32_t strlenSlot,
                       uint32_t errorLabel) {
    gen.movMemReg(X64Reg::RBP, kBytesWrittenOff, srcReg);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.callIATSlot(strlenSlot);
    gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(strlenSlot);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesReadOff);
    gen.addRegReg(X64Reg::RAX, X64Reg::RDX);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kBytesWrittenOff);
    gen.callIATSlot(catSlot);
}

/// @brief Emit a CreateDirectoryW call for the path already in RCX.
/// Treats ERROR_ALREADY_EXISTS as success; jumps to errorLabel on any other failure.
/// @param gen Instruction emitter with RCX prepared.
/// @param createDirSlot `CreateDirectoryW` IAT slot.
/// @param getLastErrorSlot `GetLastError` IAT slot.
/// @param errorLabel Failure branch target.
void emitCreateDirectoryChecked(InstallerStubGen &gen,
                                uint32_t createDirSlot,
                                uint32_t getLastErrorSlot,
                                uint32_t errorLabel) {
    const auto lblOk = gen.newLabel();
    gen.callIATSlot(createDirSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblOk);
    gen.callIATSlot(getLastErrorSlot);
    gen.cmpRegImm32(X64Reg::RAX, kErrorAlreadyExists);
    gen.jnz(errorLabel);
    gen.bindLabel(lblOk);
}

/// @brief Emit `cmp reg, value` using R10 as scratch for the immediate.
/// The direct cmp-reg-imm32 encoding sign-extends the immediate; loading it into R10
/// first allows correct unsigned comparison for values with bit 31 set.
/// @param gen Instruction emitter.
/// @param reg Register to compare.
/// @param value Unsigned 32-bit comparison value.
void emitCmpRegU32(InstallerStubGen &gen, X64Reg reg, uint32_t value) {
    gen.movRegImm32(X64Reg::R10, value);
    gen.cmpRegReg(reg, X64Reg::R10);
}

/// @brief Emit code to compose a full path into [RBP+tempOff].
/// Copies the resolved root path (install/desktop/menu) for `root`, then appends
/// "\" and the embedded relative path string; jumps to errorLabel on overflow.
/// @param gen Instruction emitter.
/// @param root Logical root whose frame buffer supplies the prefix.
/// @param tempOff Destination frame-buffer offset.
/// @param slashOff Embedded separator string offset.
/// @param relPathOff Embedded relative-path string offset.
/// @param copySlot `lstrcpyW` IAT slot.
/// @param catSlot `lstrcatW` IAT slot.
/// @param strlenSlot `lstrlenW` IAT slot.
/// @param errorLabel Overflow branch target.
void emitComposePath(InstallerStubGen &gen,
                     WindowsInstallRoot root,
                     int32_t tempOff,
                     uint32_t slashOff,
                     uint32_t relPathOff,
                     uint32_t copySlot,
                     uint32_t catSlot,
                     uint32_t strlenSlot,
                     uint32_t errorLabel) {
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, tempOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, rootBufferOffset(root));
    gen.callIATSlot(copySlot);

    emitCheckedCatEmbedded(gen, tempOff, slashOff, catSlot, strlenSlot, errorLabel);
    emitCheckedCatEmbedded(gen, tempOff, relPathOff, catSlot, strlenSlot, errorLabel);
}

/// @brief Emit a MessageBoxW call with a NULL parent window using embedded title and message
/// strings. `flags` selects icon/button style (e.g. MB_ICONINFORMATION=0x40, MB_ICONERROR=0x10).
/// @param gen Instruction emitter.
/// @param slot `MessageBoxW` IAT slot.
/// @param titleOff Embedded wide title offset.
/// @param messageOff Embedded wide message offset.
/// @param flags Win32 message-box style flags.
void emitMessageBox(
    InstallerStubGen &gen, uint32_t slot, uint32_t titleOff, uint32_t messageOff, uint32_t flags) {
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRipData(X64Reg::RDX, messageOff);
    gen.leaRipData(X64Reg::R8, titleOff);
    gen.movRegImm32(X64Reg::R9, flags);
    gen.callIATSlot(slot);
}

/// @brief Emit a MessageBoxW call unless `/quiet` or `/silent` was detected.
/// @param gen Instruction emitter.
/// @param slot `MessageBoxW` IAT slot.
/// @param titleOff Embedded wide title offset.
/// @param messageOff Embedded wide message offset.
/// @param flags Win32 message-box style flags.
void emitMessageBoxUnlessQuiet(
    InstallerStubGen &gen, uint32_t slot, uint32_t titleOff, uint32_t messageOff, uint32_t flags) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kQuietModeOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblSkip);
    emitMessageBox(gen, slot, titleOff, messageOff, flags);
    gen.bindLabel(lblSkip);
}

/// @brief Describes an embedded command-line flag string for token detection.
struct FlagModeSpec {
    uint32_t stringOff;    ///< Offset of the UTF-16 flag text in the data section.
    uint32_t utf16ByteLen; ///< Length of that flag text in bytes (UTF-16).
};

/// @brief Detect automation flags using case-insensitive token-boundary checks.
/// The stub stays dependency-free but avoids matching flag text inside the executable
/// path or inside longer arguments.
/// @param gen Instruction emitter.
/// @param getCommandLineSlot `GetCommandLineW` IAT slot.
/// @param strstrSlot `StrStrIW` IAT slot.
/// @param flags Embedded flag spellings and byte lengths to search.
/// @param destModeOff Frame-local result offset.
/// @param foundValue Value stored for a detected flag.
/// @param clearDestination Whether to zero the result before searching.
void emitDetectFlagMode(InstallerStubGen &gen,
                        uint32_t getCommandLineSlot,
                        uint32_t strstrSlot,
                        const std::vector<FlagModeSpec> &flags,
                        int32_t destModeOff,
                        uint32_t foundValue = 1,
                        bool clearDestination = true) {
    const auto lblDone = gen.newLabel();
    const auto lblFound = gen.newLabel();

    if (clearDestination)
        zeroLocalQword(gen, destModeOff);
    zeroLocalQword(gen, kCommandLineOff);

    gen.callIATSlot(getCommandLineSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movMemReg(X64Reg::RBP, kCommandLineOff, X64Reg::RAX);

    for (const auto &flag : flags) {
        const auto lblFindNext = gen.newLabel();
        const auto lblCheckEnd = gen.newLabel();
        const auto lblStartOk = gen.newLabel();
        const auto lblInvalidMatch = gen.newLabel();
        const auto lblNextFlag = gen.newLabel();
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kCommandLineOff);
        gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);

        gen.bindLabel(lblFindNext);
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kBytesReadOff);
        gen.leaRipData(X64Reg::RDX, flag.stringOff);
        gen.callIATSlot(strstrSlot);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblNextFlag);
        gen.movMemReg(X64Reg::RBP, kBytesWrittenOff, X64Reg::RAX);

        gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCommandLineOff);
        gen.cmpRegReg(X64Reg::RAX, X64Reg::R10);
        gen.jz(lblCheckEnd);
        gen.movRegReg(X64Reg::R10, X64Reg::RAX);
        gen.subRegImm32(X64Reg::R10, 2);
        gen.xorRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.movzxRegMemIndex16(X64Reg::R11, X64Reg::R10, X64Reg::RAX, 0, 0);
        gen.cmpRegImm32(X64Reg::R11, ' ');
        gen.jz(lblStartOk);
        gen.cmpRegImm32(X64Reg::R11, '\t');
        gen.jz(lblStartOk);
        gen.cmpRegImm32(X64Reg::R11, '"');
        gen.jnz(lblInvalidMatch);

        gen.bindLabel(lblStartOk);
        gen.bindLabel(lblCheckEnd);
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
        gen.addRegImm32(X64Reg::RAX, flag.utf16ByteLen);
        gen.xorRegReg(X64Reg::R10, X64Reg::R10);
        gen.movzxRegMemIndex16(X64Reg::R11, X64Reg::RAX, X64Reg::R10, 0, 0);
        gen.testRegReg(X64Reg::R11, X64Reg::R11);
        gen.jz(lblFound);
        gen.cmpRegImm32(X64Reg::R11, ' ');
        gen.jz(lblFound);
        gen.cmpRegImm32(X64Reg::R11, '\t');
        gen.jz(lblFound);
        gen.cmpRegImm32(X64Reg::R11, '"');
        gen.jz(lblFound);

        gen.bindLabel(lblInvalidMatch);
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
        gen.addRegImm32(X64Reg::RAX, 2);
        gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);
        gen.jmp(lblFindNext);
        gen.bindLabel(lblNextFlag);
    }
    gen.jmp(lblDone);

    gen.bindLabel(lblFound);
    gen.movRegImm32(X64Reg::RAX, foundValue);
    gen.movMemReg(X64Reg::RBP, destModeOff, X64Reg::RAX);
    gen.bindLabel(lblDone);
}

/// @brief Detect quiet automation flags.
/// @param gen Instruction emitter.
/// @param getCommandLineSlot `GetCommandLineW` IAT slot.
/// @param strstrSlot `StrStrIW` IAT slot.
/// @param flags Quiet/silent spellings to detect.
void emitDetectQuietMode(InstallerStubGen &gen,
                         uint32_t getCommandLineSlot,
                         uint32_t strstrSlot,
                         const std::vector<FlagModeSpec> &flags) {
    emitDetectFlagMode(gen, getCommandLineSlot, strstrSlot, flags, kQuietModeOff);
}

/// @brief Initialize default component choices and honor quiet/interactive `/no-<id>` flags.
/// @param gen Instruction emitter.
/// @param layout Optional component definitions and defaults.
/// @param getCommandLineSlot `GetCommandLineW` IAT slot.
/// @param strstrSlot `StrStrIW` IAT slot.
void emitInitializeComponentSelections(InstallerStubGen &gen,
                                       const WindowsPackageLayout &layout,
                                       uint32_t getCommandLineSlot,
                                       uint32_t strstrSlot) {
    for (size_t i = 0; i < layout.optionalComponents.size(); ++i) {
        const auto &component = layout.optionalComponents[i];
        gen.movRegImm32(X64Reg::RAX, component.defaultSelected ? 1u : 0u);
        gen.movMemReg(X64Reg::RBP,
                      kComponentSelectionOff + static_cast<int32_t>(i * sizeof(uint64_t)),
                      X64Reg::RAX);
        const std::string slashFlag = "/no-" + component.id;
        const std::string dashFlag = "-no-" + component.id;
        emitDetectFlagMode(
            gen,
            getCommandLineSlot,
            strstrSlot,
            {{gen.embedStringW(slashFlag), static_cast<uint32_t>(slashFlag.size() * 2u)},
             {gen.embedStringW(dashFlag), static_cast<uint32_t>(dashFlag.size() * 2u)}},
            kComponentSelectionOff + static_cast<int32_t>(i * sizeof(uint64_t)),
            0,
            false);
    }
}

/// @brief Publish final component choices to the transaction backend's inherited environment.
/// @param gen Instruction emitter.
/// @param layout Optional component identifiers.
/// @param setEnvironmentSlot `SetEnvironmentVariableW` IAT slot.
/// @param errorLabel Environment-update failure target.
void emitComponentSelectionEnvironment(InstallerStubGen &gen,
                                       const WindowsPackageLayout &layout,
                                       uint32_t setEnvironmentSlot,
                                       uint32_t errorLabel) {
    if (layout.optionalComponents.empty())
        return;
    const uint32_t zeroOff = gen.embedStringW("0");
    const uint32_t oneOff = gen.embedStringW("1");
    for (size_t i = 0; i < layout.optionalComponents.size(); ++i) {
        const uint32_t nameOff =
            gen.embedStringW(componentEnvironmentName(layout.optionalComponents[i].id));
        const auto lblZero = gen.newLabel();
        const auto lblSet = gen.newLabel();
        const auto lblDone = gen.newLabel();
        gen.movRegMem(X64Reg::RAX,
                      X64Reg::RBP,
                      kComponentSelectionOff + static_cast<int32_t>(i * sizeof(uint64_t)));
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblZero);
        gen.leaRipData(X64Reg::RDX, oneOff);
        gen.jmp(lblSet);
        gen.bindLabel(lblZero);
        gen.leaRipData(X64Reg::RDX, zeroOff);
        gen.bindLabel(lblSet);
        gen.leaRipData(X64Reg::RCX, nameOff);
        gen.callIATSlot(setEnvironmentSlot);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(errorLabel);
        gen.bindLabel(lblDone);
    }
}

/// @brief Compose the selected external shortcut ownership records into a stack string.
/// @param gen Instruction emitter.
/// @param layout External files and component ownership metadata.
/// @param destinationOff Destination frame-buffer offset.
/// @param copySlot `lstrcpyW` IAT slot.
/// @param catSlot `lstrcatW` IAT slot.
/// @param strlenSlot `lstrlenW` IAT slot.
/// @param errorLabel Invalid-component or length failure target.
void emitComposeSelectedShortcutOwnership(InstallerStubGen &gen,
                                          const WindowsPackageLayout &layout,
                                          int32_t destinationOff,
                                          uint32_t copySlot,
                                          uint32_t catSlot,
                                          uint32_t strlenSlot,
                                          uint32_t errorLabel) {
    const uint32_t emptyOff = gen.embedStringW("");
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destinationOff);
    gen.leaRipData(X64Reg::RDX, emptyOff);
    gen.callIATSlot(copySlot);

    for (const auto &file : layout.installFiles) {
        if (file.root == WindowsInstallRoot::InstallDir)
            continue;
        const auto lblSkip = gen.newLabel();
        if (!file.componentId.empty()) {
            /// @brief Locate the optional-component definition referenced by a shortcut.
            /// @param component Component definition to inspect.
            /// @return `true` when its identifier matches the shortcut's component identifier.
            const auto it = std::find_if(layout.optionalComponents.begin(),
                                         layout.optionalComponents.end(),
                                         [&](const WindowsOptionalComponent &component) {
                                             return component.id == file.componentId;
                                         });
            if (it == layout.optionalComponents.end())
                throw std::runtime_error("shortcut references unknown Windows component: " +
                                         file.componentId);
            const size_t index =
                static_cast<size_t>(std::distance(layout.optionalComponents.begin(), it));
            gen.movRegMem(X64Reg::RAX,
                          X64Reg::RBP,
                          kComponentSelectionOff + static_cast<int32_t>(index * sizeof(uint64_t)));
            gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
            gen.jz(lblSkip);
        }
        const std::string line = std::string(powershellRootCode(file.root)) + "|" +
                                 powershellRelPath(file.relativePath) + "\n";
        emitCheckedCatEmbedded(
            gen, destinationOff, gen.embedStringW(line), catSlot, strlenSlot, errorLabel);
        gen.bindLabel(lblSkip);
    }
}

/// @brief Emit interactive wizard invocation and result branching.
/// @param gen Instruction emitter.
/// @param templateOff Embedded dialog template offset.
/// @param dialogProcLabel Generated dialog procedure label.
/// @param initCommonControlsSlot `InitCommonControlsEx` IAT slot.
/// @param dialogBoxSlot `DialogBoxIndirectParamW` IAT slot.
/// @param cancelLabel User-cancellation branch target.
/// @param errorLabel Dialog-creation failure target.
void emitWizardDialog(InstallerStubGen &gen,
                      uint32_t templateOff,
                      uint32_t dialogProcLabel,
                      uint32_t initCommonControlsSlot,
                      uint32_t dialogBoxSlot,
                      uint32_t cancelLabel,
                      uint32_t errorLabel) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kQuietModeOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblSkip);

    gen.movMemImm32(X64Reg::RBP, kInitCommonControlsOff, 8);
    gen.movMemImm32(X64Reg::RBP, kInitCommonControlsOff + 4, kIccProgressClass);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kInitCommonControlsOff);
    gen.callIATSlot(initCommonControlsSlot);

    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRipData(X64Reg::RDX, templateOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaCodeLabel(X64Reg::R9, dialogProcLabel);
    storeStackPtrToLocal(gen, 0x20, kComponentSelectionOff);
    gen.callIATSlot(dialogBoxSlot);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdOk);
    gen.jz(lblSkip);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdCancel);
    gen.jz(cancelLabel);
    gen.jmp(errorLabel);
    gen.bindLabel(lblSkip);
}

/// @brief Show a themed informational dialog in interactive mode and ignore its result.
/// @param gen Instruction emitter.
/// @param templateOff Embedded result-dialog template offset.
/// @param dialogProcLabel Generated dialog procedure label.
/// @param dialogBoxSlot `DialogBoxIndirectParamW` IAT slot.
void emitResultDialogUnlessQuiet(InstallerStubGen &gen,
                                 uint32_t templateOff,
                                 uint32_t dialogProcLabel,
                                 uint32_t dialogBoxSlot) {
    const auto lblSkip = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kQuietModeOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblSkip);
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRipData(X64Reg::RDX, templateOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaCodeLabel(X64Reg::R9, dialogProcLabel);
    storeStackImm64(gen, 0x20, 0);
    gen.callIATSlot(dialogBoxSlot);
    gen.bindLabel(lblSkip);
}

/// @brief Update the visible progress dialog with a native setup phase.
/// @param gen Instruction emitter.
/// @param percent Progress-bar position.
/// @param statusOff Embedded wide status string offset.
void emitSetProgressUi(InstallerStubGen &gen, uint32_t percent, uint32_t statusOff) {
    const auto lblDone = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kProgressHwndOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RDX, kDlgIdProgressStatus);
    gen.leaRipData(X64Reg::R8, statusOff);
    gen.callIATSlot(kI_SetDlgItemTextW);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProgressHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdProgressBar);
    gen.callIATSlot(kI_GetDlgItem);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RDX, kPbmSetPos);
    gen.movRegImm32(X64Reg::R8, percent);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    gen.callIATSlot(kI_SendMessageW);
    gen.bindLabel(lblDone);
}

/// @brief Refresh progress controls from the transaction backend's small INI status file.
/// @param gen Instruction emitter.
/// @param sectionOff Embedded INI section name.
/// @param percentKeyOff Embedded percent key.
/// @param statusKeyOff Embedded status key.
/// @param defaultStatusOff Embedded fallback status.
/// @param defaultPercent Fallback numeric position.
void emitRefreshProgressUi(InstallerStubGen &gen,
                           uint32_t sectionOff,
                           uint32_t percentKeyOff,
                           uint32_t statusKeyOff,
                           uint32_t defaultStatusOff,
                           uint32_t defaultPercent) {
    const auto lblDone = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kProgressHwndOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);

    gen.leaRipData(X64Reg::RCX, sectionOff);
    gen.leaRipData(X64Reg::RDX, percentKeyOff);
    gen.movRegImm32(X64Reg::R8, defaultPercent);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_GetPrivateProfileIntW);
    gen.movMemReg(X64Reg::RBP, kBytesReadOff, X64Reg::RAX);

    gen.leaRipData(X64Reg::RCX, sectionOff);
    gen.leaRipData(X64Reg::RDX, statusKeyOff);
    gen.leaRipData(X64Reg::R8, defaultStatusOff);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kUninstallPathOff);
    storeStackImm64(gen, 0x20, 1024);
    storeStackPtrToLocal(gen, 0x28, kTempPathOff);
    gen.callIATSlot(kI_GetPrivateProfileStringW);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProgressHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdProgressStatus);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kUninstallPathOff);
    gen.callIATSlot(kI_SetDlgItemTextW);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProgressHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdProgressBar);
    gen.callIATSlot(kI_GetDlgItem);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RDX, kPbmSetPos);
    gen.movRegMem(X64Reg::R8, X64Reg::RBP, kBytesReadOff);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    gen.callIATSlot(kI_SendMessageW);
    gen.bindLabel(lblDone);
}

/// @brief Drain the UI message queue so the modeless progress window remains responsive.
/// @param gen Instruction emitter.
void emitPumpProgressMessages(InstallerStubGen &gen) {
    const auto lblLoop = gen.newLabel();
    const auto lblDone = gen.newLabel();
    gen.bindLabel(lblLoop);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMessageStructOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    storeStackImm64(gen, 0x20, kPmRemove);
    gen.callIATSlot(kI_PeekMessageW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMessageStructOff);
    gen.callIATSlot(kI_TranslateMessage);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMessageStructOff);
    gen.callIATSlot(kI_DispatchMessageW);
    gen.jmp(lblLoop);
    gen.bindLabel(lblDone);
}

/// @brief Create and show the persistent progress dialog and publish its status-file path.
/// @param gen Instruction emitter.
/// @param layout Package identity used for the progress filename.
/// @param templateOff Embedded progress-dialog template offset.
/// @param dialogProcLabel Generated dialog procedure label.
/// @param errorLabel Setup failure branch target.
void emitCreateProgressDialog(InstallerStubGen &gen,
                              const WindowsPackageLayout &layout,
                              uint32_t templateOff,
                              uint32_t dialogProcLabel,
                              uint32_t errorLabel) {
    const uint32_t progressEnvironmentOff = gen.embedStringW("ZANNA_INSTALLER_PROGRESS");
    const uint32_t progressFileOff =
        gen.embedStringW("ZannaInstaller-" + registryIdFor(layout) + ".progress.ini");
    const auto lblSkipDialog = gen.newLabel();
    const auto lblDone = gen.newLabel();
    zeroLocalQword(gen, kProgressHwndOff);
    gen.movRegImm32(X64Reg::RCX, kMaxPathChars);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_GetTempPathW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(errorLabel);
    emitCheckedCatEmbedded(
        gen, kTempPathOff, progressFileOff, kI_lstrcatW, kI_lstrlenW, errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_DeleteFileW);
    gen.leaRipData(X64Reg::RCX, progressEnvironmentOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_SetEnvironmentVariableW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movRegImm32(X64Reg::RAX, 1);
    gen.movMemReg(X64Reg::RBP, kProgressPathReadyOff, X64Reg::RAX);

    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kQuietModeOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblSkipDialog);
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRipData(X64Reg::RDX, templateOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaCodeLabel(X64Reg::R9, dialogProcLabel);
    storeStackImm64(gen, 0x20, 1);
    gen.callIATSlot(kI_CreateDialogIndirectParamW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movMemReg(X64Reg::RBP, kProgressHwndOff, X64Reg::RAX);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RDX, kSwShow);
    gen.callIATSlot(kI_ShowWindow);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProgressHwndOff);
    gen.callIATSlot(kI_UpdateWindow);
    gen.bindLabel(lblSkipDialog);
    gen.bindLabel(lblDone);
}

/// @brief Tear down the progress window and remove its transient status file.
/// @param gen Instruction emitter.
/// @param layout Package identity used for the progress filename.
void emitDestroyProgressDialog(InstallerStubGen &gen, const WindowsPackageLayout &layout) {
    const auto lblNoWindow = gen.newLabel();
    const auto lblDone = gen.newLabel();
    const uint32_t progressFileOff =
        gen.embedStringW("ZannaInstaller-" + registryIdFor(layout) + ".progress.ini");
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProgressHwndOff);
    gen.testRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.jz(lblNoWindow);
    gen.callIATSlot(kI_DestroyWindow);
    zeroLocalQword(gen, kProgressHwndOff);
    gen.bindLabel(lblNoWindow);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kProgressPathReadyOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movRegImm32(X64Reg::RCX, kMaxPathChars);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_GetTempPathW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(lblDone);
    emitCheckedCatEmbedded(gen, kTempPathOff, progressFileOff, kI_lstrcatW, kI_lstrlenW, lblDone);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(kI_DeleteFileW);
    zeroLocalQword(gen, kProgressPathReadyOff);
    gen.bindLabel(lblDone);
}

/// @brief Emit EndDialog followed by a true dialog-procedure result.
/// @param gen Instruction emitter.
/// @param endDialogSlot `EndDialog` IAT slot.
/// @param result Modal result passed to the caller.
void emitEndDialogAndReturnTrue(InstallerStubGen &gen, uint32_t endDialogSlot, uint32_t result) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, result);
    gen.callIATSlot(endDialogSlot);
    gen.movRegImm32(X64Reg::RAX, 1);
}

/// @brief Emit the shared dialog-procedure stack teardown and return.
/// @param gen Instruction emitter.
void emitDialogProcEpilogue(InstallerStubGen &gen) {
    gen.addRegImm32(X64Reg::RSP, kDlgProcFrameSize);
    gen.pop(X64Reg::RBX);
    gen.pop(X64Reg::RBP);
    gen.ret();
}

/// @brief Apply the native dark Explorer theme to one dialog child when it exists.
/// @param gen Instruction emitter.
/// @param controlId Child control identifier.
/// @param darkThemeOff Embedded theme-name offset.
/// @param getDlgItemSlot `GetDlgItem` IAT slot.
/// @param setWindowThemeSlot `SetWindowTheme` IAT slot.
void emitSetDialogControlTheme(InstallerStubGen &gen,
                               uint32_t controlId,
                               uint32_t darkThemeOff,
                               uint32_t getDlgItemSlot,
                               uint32_t setWindowThemeSlot) {
    const auto lblDone = gen.newLabel();
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, controlId);
    gen.callIATSlot(getDlgItemSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.leaRipData(X64Reg::RDX, darkThemeOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.callIATSlot(setWindowThemeSlot);
    gen.bindLabel(lblDone);
}

/// @brief Paint one dialog/control surface with a stock DC brush and return that brush.
/// @param gen Instruction emitter.
/// @param background Win32 COLORREF background.
/// @param foreground Win32 COLORREF text color.
/// @param getStockObjectSlot `GetStockObject` IAT slot.
/// @param setDcBrushColorSlot `SetDCBrushColor` IAT slot.
/// @param setTextColorSlot `SetTextColor` IAT slot.
/// @param setBkColorSlot `SetBkColor` IAT slot.
/// @param doneLabel Shared return branch target.
void emitReturnDarkDialogBrush(InstallerStubGen &gen,
                               uint32_t background,
                               uint32_t foreground,
                               uint32_t getStockObjectSlot,
                               uint32_t setDcBrushColorSlot,
                               uint32_t setTextColorSlot,
                               uint32_t setBkColorSlot,
                               uint32_t doneLabel) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcWparamOff);
    gen.movRegImm32(X64Reg::RDX, foreground);
    gen.callIATSlot(setTextColorSlot);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcWparamOff);
    gen.movRegImm32(X64Reg::RDX, background);
    gen.callIATSlot(setBkColorSlot);
    gen.movRegImm32(X64Reg::RCX, kDcBrush);
    gen.callIATSlot(getStockObjectSlot);
    gen.movRegReg(X64Reg::RBX, X64Reg::RAX);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcWparamOff);
    gen.movRegImm32(X64Reg::RDX, background);
    gen.callIATSlot(setDcBrushColorSlot);
    gen.movRegReg(X64Reg::RAX, X64Reg::RBX);
    gen.jmp(doneLabel);
}

/// @brief Emit the shared modal/modeless wizard window procedure.
/// @param gen Instruction emitter.
/// @param layout Component controls and optional banner artwork.
/// @param label Entry label bound to the generated procedure.
/// @param endDialogSlot `EndDialog` IAT slot.
/// @param getDlgItemSlot `GetDlgItem` IAT slot.
/// @param sendMessageSlot `SendMessageW` IAT slot.
/// @param enableWindowSlot `EnableWindow` IAT slot.
/// @param darkThemeOff Embedded dark-theme name.
/// @param getDlgCtrlIdSlot `GetDlgCtrlID` IAT slot.
/// @param setWindowThemeSlot `SetWindowTheme` IAT slot.
/// @param dwmSetWindowAttributeSlot `DwmSetWindowAttribute` IAT slot.
/// @param getStockObjectSlot `GetStockObject` IAT slot.
/// @param setDcBrushColorSlot `SetDCBrushColor` IAT slot.
/// @param setTextColorSlot `SetTextColor` IAT slot.
/// @param setBkColorSlot `SetBkColor` IAT slot.
/// @param setWindowLongPtrSlot Optional `SetWindowLongPtrW` slot for enhanced dialogs.
/// @param getWindowLongPtrSlot Optional `GetWindowLongPtrW` slot for enhanced dialogs.
/// @param stretchDibitsSlot Optional `StretchDIBits` slot for branded banners.
/// @param dibInfoOff Embedded BITMAPINFOHEADER offset.
/// @param dibPixelsOff Embedded BGRA pixel offset.
/// @details Handles initialization, commands, closure, dark control colors,
///          optional owner-drawn artwork, and component-selection persistence.
void emitWizardDialogProc(InstallerStubGen &gen,
                          const WindowsPackageLayout &layout,
                          uint32_t label,
                          uint32_t endDialogSlot,
                          uint32_t getDlgItemSlot,
                          uint32_t sendMessageSlot,
                          uint32_t enableWindowSlot,
                          uint32_t darkThemeOff,
                          uint32_t getDlgCtrlIdSlot,
                          uint32_t setWindowThemeSlot,
                          uint32_t dwmSetWindowAttributeSlot,
                          uint32_t getStockObjectSlot,
                          uint32_t setDcBrushColorSlot,
                          uint32_t setTextColorSlot,
                          uint32_t setBkColorSlot,
                          uint32_t setWindowLongPtrSlot = UINT32_MAX,
                          uint32_t getWindowLongPtrSlot = UINT32_MAX,
                          uint32_t stretchDibitsSlot = UINT32_MAX,
                          uint32_t dibInfoOff = 0,
                          uint32_t dibPixelsOff = 0) {
    const bool enhancedDialog =
        setWindowLongPtrSlot != UINT32_MAX && getWindowLongPtrSlot != UINT32_MAX;
    const bool brandedBanner = stretchDibitsSlot != UINT32_MAX && !layout.wizardImageRgba.empty();
    const auto lblInit = gen.newLabel();
    const auto lblCommand = gen.newLabel();
    const auto lblClose = gen.newLabel();
    const auto lblDrawItem = gen.newLabel();
    const auto lblCtlColorEdit = gen.newLabel();
    const auto lblCtlColorButton = gen.newLabel();
    const auto lblCtlColorDialog = gen.newLabel();
    const auto lblCtlColorStatic = gen.newLabel();
    const auto lblInitResult = gen.newLabel();
    const auto lblInitProgress = gen.newLabel();
    const auto lblInitWizard = gen.newLabel();
    const auto lblInitSetFocus = gen.newLabel();
    const auto lblStaticBanner = gen.newLabel();
    const auto lblStaticLicense = gen.newLabel();
    const auto lblStaticMuted = gen.newLabel();
    const auto lblAccept = gen.newLabel();
    const auto lblOk = gen.newLabel();
    const auto lblCancel = gen.newLabel();
    const auto lblReturnTrue = gen.newLabel();
    const auto lblReturnFalse = gen.newLabel();
    const auto lblDone = gen.newLabel();

    gen.bindLabel(label);
    gen.push(X64Reg::RBP);
    gen.push(X64Reg::RBX);
    gen.movRegReg(X64Reg::RBP, X64Reg::RSP);
    gen.subRegImm32(X64Reg::RSP, kDlgProcFrameSize);
    gen.movMemReg(X64Reg::RBP, kDlgProcHwndOff, X64Reg::RCX);
    gen.movMemReg(X64Reg::RBP, kDlgProcMsgOff, X64Reg::RDX);
    gen.movMemReg(X64Reg::RBP, kDlgProcWparamOff, X64Reg::R8);
    gen.movMemReg(X64Reg::RBP, kDlgProcLparamOff, X64Reg::R9);

    gen.cmpRegImm32(X64Reg::RDX, kWmInitDialog);
    gen.jz(lblInit);
    gen.cmpRegImm32(X64Reg::RDX, kWmCommand);
    gen.jz(lblCommand);
    gen.cmpRegImm32(X64Reg::RDX, kWmClose);
    gen.jz(lblClose);
    if (brandedBanner) {
        gen.cmpRegImm32(X64Reg::RDX, kWmDrawItem);
        gen.jz(lblDrawItem);
    }
    gen.cmpRegImm32(X64Reg::RDX, kWmCtlColorEdit);
    gen.jz(lblCtlColorEdit);
    gen.cmpRegImm32(X64Reg::RDX, kWmCtlColorButton);
    gen.jz(lblCtlColorButton);
    gen.cmpRegImm32(X64Reg::RDX, kWmCtlColorDialog);
    gen.jz(lblCtlColorDialog);
    gen.cmpRegImm32(X64Reg::RDX, kWmCtlColorStatic);
    gen.jz(lblCtlColorStatic);
    gen.jmp(lblReturnFalse);

    gen.bindLabel(lblInit);
    gen.movMemImm32(X64Reg::RBP, kDlgProcDarkModeOff, 1);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDwmUseImmersiveDarkMode);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kDlgProcDarkModeOff);
    gen.movRegImm32(X64Reg::R9, 4);
    gen.callIATSlot(dwmSetWindowAttributeSlot);
    const auto lblDarkTitleDone = gen.newLabel();
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDarkTitleDone);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDwmUseImmersiveDarkModeBefore20H1);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kDlgProcDarkModeOff);
    gen.movRegImm32(X64Reg::R9, 4);
    gen.callIATSlot(dwmSetWindowAttributeSlot);
    gen.bindLabel(lblDarkTitleDone);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.leaRipData(X64Reg::RDX, darkThemeOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.callIATSlot(setWindowThemeSlot);
    emitSetDialogControlTheme(gen, kDlgIdLicense, darkThemeOff, getDlgItemSlot, setWindowThemeSlot);
    emitSetDialogControlTheme(gen, kDlgIdAccept, darkThemeOff, getDlgItemSlot, setWindowThemeSlot);
    emitSetDialogControlTheme(gen, kDlgIdOk, darkThemeOff, getDlgItemSlot, setWindowThemeSlot);
    emitSetDialogControlTheme(gen, kDlgIdCancel, darkThemeOff, getDlgItemSlot, setWindowThemeSlot);
    emitSetDialogControlTheme(
        gen, kDlgIdProgressBar, darkThemeOff, getDlgItemSlot, setWindowThemeSlot);
    for (size_t i = 0; i < layout.optionalComponents.size(); ++i) {
        emitSetDialogControlTheme(gen,
                                  kDlgIdComponentBase + static_cast<uint32_t>(i),
                                  darkThemeOff,
                                  getDlgItemSlot,
                                  setWindowThemeSlot);
    }
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kDlgProcLparamOff);
    if (enhancedDialog) {
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
        gen.movRegImm64(X64Reg::RDX, static_cast<uint64_t>(static_cast<int64_t>(-21)));
        gen.movRegReg(X64Reg::R8, X64Reg::RAX);
        gen.callIATSlot(setWindowLongPtrSlot);
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kDlgProcLparamOff);
    }
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblInitResult);
    gen.cmpRegImm32(X64Reg::RAX, 1);
    gen.jz(lblInitProgress);
    gen.jmp(lblInitWizard);

    gen.bindLabel(lblInitWizard);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdOk);
    gen.callIATSlot(getDlgItemSlot);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.callIATSlot(enableWindowSlot);
    if (enhancedDialog) {
        for (size_t i = 0; i < layout.optionalComponents.size(); ++i) {
            gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
            gen.movRegImm32(X64Reg::RDX, kDlgIdComponentBase + static_cast<uint32_t>(i));
            gen.callIATSlot(getDlgItemSlot);
            gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
            gen.movRegImm32(X64Reg::RDX, kBmSetCheck);
            gen.movRegMem(X64Reg::R8, X64Reg::RBP, kDlgProcLparamOff);
            gen.movRegMem(X64Reg::R8, X64Reg::R8, static_cast<int32_t>(i * sizeof(uint64_t)));
            gen.xorRegReg(X64Reg::R9, X64Reg::R9);
            gen.callIATSlot(sendMessageSlot);
        }
    }
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdAccept);
    gen.jmp(lblInitSetFocus);

    gen.bindLabel(lblInitResult);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdOk);
    gen.jmp(lblInitSetFocus);

    gen.bindLabel(lblInitProgress);
    gen.jmp(lblReturnTrue);

    gen.bindLabel(lblInitSetFocus);
    gen.callIATSlot(getDlgItemSlot);
    gen.movRegReg(X64Reg::RBX, X64Reg::RAX);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kWmNextDlgCtl);
    gen.movRegReg(X64Reg::R8, X64Reg::RBX);
    gen.movRegImm32(X64Reg::R9, 1);
    gen.callIATSlot(sendMessageSlot);
    gen.jmp(lblReturnFalse);

    if (brandedBanner) {
        gen.bindLabel(lblDrawItem);
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kDlgProcLparamOff);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblReturnFalse);
        gen.movRegMem32(X64Reg::R10, X64Reg::RAX, 4); // DRAWITEMSTRUCT::CtlID
        gen.cmpRegImm32(X64Reg::R10, kDlgIdBanner);
        gen.jnz(lblReturnFalse);

        gen.movRegMem(X64Reg::RCX, X64Reg::RAX, 32); // DRAWITEMSTRUCT::hDC
        gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
        gen.xorRegReg(X64Reg::R8, X64Reg::R8);
        gen.movRegMem32(X64Reg::R9, X64Reg::RAX, 48);  // rcItem.right
        gen.movRegMem32(X64Reg::R10, X64Reg::RAX, 40); // rcItem.left
        gen.subRegReg(X64Reg::R9, X64Reg::R10);
        gen.movRegMem32(X64Reg::R10, X64Reg::RAX, 52); // rcItem.bottom
        gen.movRegMem32(X64Reg::R11, X64Reg::RAX, 44); // rcItem.top
        gen.subRegReg(X64Reg::R10, X64Reg::R11);
        gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::R10);
        storeStackImm64(gen, 0x28, 0);
        storeStackImm64(gen, 0x30, 0);
        storeStackImm64(gen, 0x38, layout.wizardImageWidth);
        storeStackImm64(gen, 0x40, layout.wizardImageHeight);
        gen.leaRipData(X64Reg::R10, dibPixelsOff);
        gen.movMemReg(X64Reg::RSP, 0x48, X64Reg::R10);
        gen.leaRipData(X64Reg::R10, dibInfoOff);
        gen.movMemReg(X64Reg::RSP, 0x50, X64Reg::R10);
        storeStackImm64(gen, 0x58, 0); // DIB_RGB_COLORS
        storeStackImm64(gen, 0x60, kSrcCopy);
        gen.callIATSlot(stretchDibitsSlot);
        gen.jmp(lblReturnTrue);
    }

    gen.bindLabel(lblCtlColorEdit);
    emitReturnDarkDialogBrush(gen,
                              kDarkSurface,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblCtlColorButton);
    emitReturnDarkDialogBrush(gen,
                              kDarkBackground,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblCtlColorDialog);
    emitReturnDarkDialogBrush(gen,
                              kDarkBackground,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblCtlColorStatic);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcLparamOff);
    gen.callIATSlot(getDlgCtrlIdSlot);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdBanner);
    gen.jz(lblStaticBanner);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdLicense);
    gen.jz(lblStaticLicense);
    gen.cmpRegImm32(X64Reg::RAX, 2003);
    gen.jz(lblStaticMuted);
    emitReturnDarkDialogBrush(gen,
                              kDarkBackground,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblStaticBanner);
    emitReturnDarkDialogBrush(gen,
                              kDarkAccent,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblStaticLicense);
    emitReturnDarkDialogBrush(gen,
                              kDarkSurface,
                              kDarkText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblStaticMuted);
    emitReturnDarkDialogBrush(gen,
                              kDarkBackground,
                              kDarkMutedText,
                              getStockObjectSlot,
                              setDcBrushColorSlot,
                              setTextColorSlot,
                              setBkColorSlot,
                              lblDone);

    gen.bindLabel(lblCommand);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kDlgProcWparamOff);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdOk);
    gen.jz(lblOk);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdCancel);
    gen.jz(lblCancel);
    gen.cmpRegImm32(X64Reg::RAX, kDlgIdAccept);
    gen.jz(lblAccept);
    gen.jmp(lblReturnFalse);

    gen.bindLabel(lblAccept);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdAccept);
    gen.callIATSlot(getDlgItemSlot);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RDX, kBmGetCheck);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    gen.callIATSlot(sendMessageSlot);
    gen.movRegReg(X64Reg::RBX, X64Reg::RAX);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
    gen.movRegImm32(X64Reg::RDX, kDlgIdOk);
    gen.callIATSlot(getDlgItemSlot);
    gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
    gen.movRegReg(X64Reg::RDX, X64Reg::RBX);
    gen.callIATSlot(enableWindowSlot);
    gen.jmp(lblReturnTrue);

    gen.bindLabel(lblOk);
    if (enhancedDialog && !layout.optionalComponents.empty()) {
        const auto lblNoSelections = gen.newLabel();
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
        gen.movRegImm64(X64Reg::RDX, static_cast<uint64_t>(static_cast<int64_t>(-21)));
        gen.callIATSlot(getWindowLongPtrSlot);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblNoSelections);
        gen.cmpRegImm32(X64Reg::RAX, 1);
        gen.jz(lblNoSelections);
        gen.movRegReg(X64Reg::RBX, X64Reg::RAX);
        for (size_t i = 0; i < layout.optionalComponents.size(); ++i) {
            gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
            gen.movRegImm32(X64Reg::RDX, kDlgIdComponentBase + static_cast<uint32_t>(i));
            gen.callIATSlot(getDlgItemSlot);
            gen.movRegReg(X64Reg::RCX, X64Reg::RAX);
            gen.movRegImm32(X64Reg::RDX, kBmGetCheck);
            gen.xorRegReg(X64Reg::R8, X64Reg::R8);
            gen.xorRegReg(X64Reg::R9, X64Reg::R9);
            gen.callIATSlot(sendMessageSlot);
            gen.movMemReg(X64Reg::RBX, static_cast<int32_t>(i * sizeof(uint64_t)), X64Reg::RAX);
        }
        gen.bindLabel(lblNoSelections);
    }
    emitEndDialogAndReturnTrue(gen, endDialogSlot, kDlgIdOk);
    gen.jmp(lblDone);

    gen.bindLabel(lblCancel);
    emitEndDialogAndReturnTrue(gen, endDialogSlot, kDlgIdCancel);
    gen.jmp(lblDone);

    gen.bindLabel(lblClose);
    if (enhancedDialog) {
        const auto lblCloseDialog = gen.newLabel();
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kDlgProcHwndOff);
        gen.movRegImm64(X64Reg::RDX, static_cast<uint64_t>(static_cast<int64_t>(-21)));
        gen.callIATSlot(getWindowLongPtrSlot);
        gen.cmpRegImm32(X64Reg::RAX, 1);
        gen.jnz(lblCloseDialog);
        gen.jmp(lblReturnTrue);
        gen.bindLabel(lblCloseDialog);
    }
    emitEndDialogAndReturnTrue(gen, endDialogSlot, kDlgIdCancel);
    gen.jmp(lblDone);

    gen.bindLabel(lblReturnTrue);
    gen.movRegImm32(X64Reg::RAX, 1);
    gen.jmp(lblDone);

    gen.bindLabel(lblReturnFalse);
    gen.xorRegReg(X64Reg::RAX, X64Reg::RAX);

    gen.bindLabel(lblDone);
    emitDialogProcEpilogue(gen);
}

/// @brief Emit code to create a directory at root\relPath.
/// Composes the full path into kTempPathOff via emitComposePath, then calls
/// CreateDirectoryW (ERROR_ALREADY_EXISTS is silently tolerated).
/// @param gen Instruction emitter.
/// @param root Logical destination root.
/// @param slashOff Embedded separator offset.
/// @param relPathOff Embedded relative-path offset.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param createDirSlot `CreateDirectoryW` slot.
/// @param getLastErrorSlot `GetLastError` slot.
/// @param errorLabel Composition or creation failure target.
void emitCreateDirectoryAtRoot(InstallerStubGen &gen,
                               WindowsInstallRoot root,
                               uint32_t slashOff,
                               uint32_t relPathOff,
                               uint32_t copySlot,
                               uint32_t catSlot,
                               uint32_t strlenSlot,
                               uint32_t createDirSlot,
                               uint32_t getLastErrorSlot,
                               uint32_t errorLabel) {
    emitComposePath(
        gen, root, kTempPathOff, slashOff, relPathOff, copySlot, catSlot, strlenSlot, errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    emitCreateDirectoryChecked(gen, createDirSlot, getLastErrorSlot, errorLabel);
}

/// @brief Emit RegSetValueExW to write a named REG_SZ value from an embedded wide string.
/// `valueBytes` is the full UTF-16LE byte count including the null terminator.
/// Jumps to errorLabel if the API returns non-zero.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param valueOff Embedded UTF-16 value offset.
/// @param valueBytes Serialized value size including NUL.
/// @param errorLabel Registry failure target.
void emitRegSetConstString(InstallerStubGen &gen,
                           uint32_t regSetSlot,
                           uint32_t valueNameOff,
                           uint32_t valueOff,
                           uint32_t valueBytes,
                           uint32_t errorLabel) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, kRegSz);
    gen.leaRipData(X64Reg::RAX, valueOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RAX, valueBytes);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegSetValueExW to write a named REG_DWORD value.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param value DWORD payload.
/// @param errorLabel Registry failure target.
void emitRegSetConstDword(InstallerStubGen &gen,
                          uint32_t regSetSlot,
                          uint32_t valueNameOff,
                          uint32_t value,
                          uint32_t errorLabel) {
    gen.movMemImm32(X64Reg::RBP, kBytesWrittenOff, value);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, kRegDword);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RAX, 4);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegSetValueExW to write a named REG_NONE value with zero length and NULL data.
/// Used to stamp a ProgID name into Software\Classes\.<ext>\OpenWithProgids.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param valueNameOff Embedded ProgID value-name offset.
/// @param errorLabel Registry failure target.
void emitRegSetConstNoneValue(InstallerStubGen &gen,
                              uint32_t regSetSlot,
                              uint32_t valueNameOff,
                              uint32_t errorLabel) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, kRegNone);
    gen.xorRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegSetValueExW to write the default (unnamed, empty-string key) value as REG_SZ.
/// Used to set the human-readable ProgID description shown in the Open With dialog.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param valueOff Embedded UTF-16 value offset.
/// @param valueBytes Serialized value size including NUL.
/// @param errorLabel Registry failure target.
void emitRegSetDefaultConstString(InstallerStubGen &gen,
                                  uint32_t regSetSlot,
                                  uint32_t valueOff,
                                  uint32_t valueBytes,
                                  uint32_t errorLabel) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, kRegSz);
    gen.leaRipData(X64Reg::RAX, valueOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.movRegImm32(X64Reg::RAX, valueBytes);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegSetValueExW to write a named value from a stack-resident wide string.
/// Computes the byte length at runtime via lstrlenW (* 2 + 2 for null terminator).
/// `valueType` selects REG_SZ or REG_EXPAND_SZ; jumps to errorLabel on failure.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param stackBufOff Frame offset of the UTF-16 value.
/// @param valueType Registry string type.
/// @param errorLabel Registry failure target.
void emitRegSetStackString(InstallerStubGen &gen,
                           uint32_t regSetSlot,
                           uint32_t strlenSlot,
                           uint32_t valueNameOff,
                           int32_t stackBufOff,
                           uint32_t valueType,
                           uint32_t errorLabel) {
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, valueType);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, stackBufOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, stackBufOff);
    gen.callIATSlot(strlenSlot);
    gen.addRegImm32(X64Reg::RAX, 1);
    gen.addRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, valueType);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Convenience overload of emitRegSetStackString that defaults `valueType` to REG_SZ.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param stackBufOff Frame offset of the UTF-16 value.
/// @param errorLabel Registry failure target.
void emitRegSetStackString(InstallerStubGen &gen,
                           uint32_t regSetSlot,
                           uint32_t strlenSlot,
                           uint32_t valueNameOff,
                           int32_t stackBufOff,
                           uint32_t errorLabel) {
    emitRegSetStackString(
        gen, regSetSlot, strlenSlot, valueNameOff, stackBufOff, kRegSz, errorLabel);
}

/// @brief Emit InstallDate as the current local date in YYYYMMDD form.
/// Falls back to the generator-supplied date if GetDateFormatW fails.
/// @param gen Instruction emitter.
/// @param getDateFormatSlot `GetDateFormatW` slot.
/// @param copySlot `lstrcpyW` slot.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param valueNameOff Embedded `InstallDate` name.
/// @param formatOff Embedded date-format string.
/// @param fallbackDateOff Embedded packaging-date fallback.
/// @param errorLabel Registry failure target.
void emitRegSetCurrentInstallDate(InstallerStubGen &gen,
                                  uint32_t getDateFormatSlot,
                                  uint32_t copySlot,
                                  uint32_t regSetSlot,
                                  uint32_t strlenSlot,
                                  uint32_t valueNameOff,
                                  uint32_t formatOff,
                                  uint32_t fallbackDateOff,
                                  uint32_t errorLabel) {
    const auto lblUseStackDate = gen.newLabel();

    gen.movRegImm32(X64Reg::RCX, kLocaleUserDefault);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRipData(X64Reg::R9, formatOff);
    storeStackPtrToLocal(gen, 0x20, kTempPathOff);
    storeStackImm64(gen, 0x28, 9);
    gen.callIATSlot(getDateFormatSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblUseStackDate);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRipData(X64Reg::RDX, fallbackDateOff);
    gen.callIATSlot(copySlot);

    gen.bindLabel(lblUseStackDate);
    emitRegSetStackString(gen, regSetSlot, strlenSlot, valueNameOff, kTempPathOff, errorLabel);
}

/// @brief Emit RegSetValueExW to write the default (unnamed) value from a stack-resident wide
/// string. Computes the byte length via lstrlenW at runtime. Used for shell Open command strings.
/// @param gen Instruction emitter.
/// @param regSetSlot `RegSetValueExW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param stackBufOff Frame offset of the UTF-16 value.
/// @param errorLabel Registry failure target.
void emitRegSetDefaultStackString(InstallerStubGen &gen,
                                  uint32_t regSetSlot,
                                  uint32_t strlenSlot,
                                  int32_t stackBufOff,
                                  uint32_t errorLabel) {
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, stackBufOff);
    gen.callIATSlot(strlenSlot);
    gen.addRegImm32(X64Reg::RAX, 1);
    gen.addRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.movRegImm32(X64Reg::R9, kRegSz);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, stackBufOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.callIATSlot(regSetSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegQueryValueExW to read a named value from kRegKeyOff into a stack buffer.
/// `bufferBytes` is the buffer's capacity in bytes. Only missing-value errors jump
/// to missingLabel when a distinct errorLabel is supplied; malformed, oversized, or
/// unreadable values jump to errorLabel so PATH updates cannot overwrite unknown data.
/// @param gen Instruction emitter.
/// @param querySlot `RegQueryValueExW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param stackBufOff Destination frame-buffer offset.
/// @param bufferBytes Writable buffer capacity.
/// @param missingLabel Missing-key/value target.
/// @param errorLabel Optional distinct malformed/query failure target.
void emitRegQueryStackString(InstallerStubGen &gen,
                             uint32_t querySlot,
                             uint32_t valueNameOff,
                             int32_t stackBufOff,
                             uint32_t bufferBytes,
                             uint32_t missingLabel,
                             uint32_t errorLabel = UINT32_MAX) {
    const auto lblTypeOk = gen.newLabel();
    const auto lblQueryOk = gen.newLabel();
    const uint32_t fatalLabel = errorLabel == UINT32_MAX ? missingLabel : errorLabel;
    gen.movMemImm32(X64Reg::RBP, stackBufOff, 0);
    gen.movMemImm32(X64Reg::RBP, stackBufOff + static_cast<int32_t>(bufferBytes) - 4, 0);
    zeroLocalQword(gen, kBytesReadOff);
    zeroLocalQword(gen, kBytesWrittenOff);
    gen.movMemImm32(X64Reg::RBP, kBytesReadOff, bufferBytes);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kBytesWrittenOff);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, stackBufOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(querySlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblQueryOk);
    if (fatalLabel == missingLabel) {
        gen.jmp(missingLabel);
    } else {
        gen.cmpRegImm32(X64Reg::RAX, kErrorFileNotFound);
        gen.jz(missingLabel);
        gen.cmpRegImm32(X64Reg::RAX, kErrorPathNotFound);
        gen.jz(missingLabel);
        gen.jmp(fatalLabel);
    }
    gen.bindLabel(lblQueryOk);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
    gen.cmpRegImm32(X64Reg::RAX, kRegSz);
    gen.jz(lblTypeOk);
    gen.cmpRegImm32(X64Reg::RAX, kRegExpandSz);
    gen.jnz(fatalLabel);
    gen.bindLabel(lblTypeOk);
    gen.movMemImm32(X64Reg::RBP, stackBufOff + static_cast<int32_t>(bufferBytes) - 4, 0);
}

/// @brief Emit code to append the separator string to [RBP+destOff] only when the buffer is
/// non-empty. Used to insert ";" between PATH tokens without creating a spurious leading separator.
/// @param gen Instruction emitter.
/// @param destOff Destination frame-buffer offset.
/// @param separatorOff Embedded separator offset.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param errorLabel Length-overflow target.
void emitAppendSeparatorIfNonEmpty(InstallerStubGen &gen,
                                   int32_t destOff,
                                   uint32_t separatorOff,
                                   uint32_t catSlot,
                                   uint32_t strlenSlot,
                                   uint32_t errorLabel) {
    const auto lblSkip = gen.newLabel();
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.callIATSlot(strlenSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblSkip);
    emitCheckedCatEmbedded(gen, destOff, separatorOff, catSlot, strlenSlot, errorLabel);
    gen.bindLabel(lblSkip);
}

/// @brief Emit SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment", ...).
/// Forces running applications (including Explorer) to reload the system environment
/// block so PATH changes take effect immediately without a reboot.
/// @param gen Instruction emitter.
/// @param sendSlot `SendMessageTimeoutW` slot.
/// @param environmentOff Embedded `"Environment"` string offset.
void emitBroadcastEnvironmentChange(InstallerStubGen &gen,
                                    uint32_t sendSlot,
                                    uint32_t environmentOff) {
    gen.movRegImm32(X64Reg::RCX, 0xFFFFu);
    gen.movRegImm32(X64Reg::RDX, 0x001Au);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRipData(X64Reg::R9, environmentOff);
    storeStackImm64(gen, 0x20, 0x0002u);
    storeStackImm64(gen, 0x28, 5000u);
    storeStackImm64(gen, 0x30, 0u);
    gen.callIATSlot(sendSlot);
}

/// @brief Emit RegCreateKeyW under a selected hive, closing any previously open key first.
/// @param gen Instruction emitter.
/// @param createSlot `RegCreateKeyW` slot.
/// @param keyOff Embedded subkey path offset.
/// @param errorLabel Creation failure target.
/// @param rootHkey Predefined registry root handle.
void emitRegCreateConstKey(InstallerStubGen &gen,
                           uint32_t createSlot,
                           uint32_t keyOff,
                           uint32_t errorLabel,
                           uint64_t rootHkey = kHkeyLocalMachine) {
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);
    gen.movRegImm64(X64Reg::RCX, rootHkey);
    gen.leaRipData(X64Reg::RDX, keyOff);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kRegKeyOff);
    gen.callIATSlot(createSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

/// @brief Emit RegDeleteTreeW for the embedded key path. Errors are silently ignored since
/// the subtree may already be absent during rollback or uninstall.
/// @param gen Instruction emitter.
/// @param deleteSlot `RegDeleteTreeW` slot.
/// @param keyOff Embedded subkey path offset.
/// @param rootHkey Predefined registry root handle.
void emitRegDeleteConstKey(InstallerStubGen &gen,
                           uint32_t deleteSlot,
                           uint32_t keyOff,
                           uint64_t rootHkey = kHkeyLocalMachine) {
    gen.movRegImm64(X64Reg::RCX, rootHkey);
    gen.leaRipData(X64Reg::RDX, keyOff);
    gen.callIATSlot(deleteSlot);
}

/// @brief Build the hive-relative Software\Classes\.<ext> key for a file association.
/// Normalizes the extension to have a leading dot if it is missing.
/// @param assoc File association supplying the extension.
/// @return Extension-class subkey path.
std::string extensionKeyFor(const WindowsFileAssociationEntry &assoc) {
    std::string ext = assoc.extension;
    if (ext.empty() || ext.front() != '.')
        ext.insert(ext.begin(), '.');
    return "Software\\Classes\\" + ext;
}

/// @brief Build the hive-relative Software\Classes\<ProgID> key for an association.
/// @param assoc File association supplying its ProgID.
/// @return ProgID subkey path.
std::string progIdKeyFor(const WindowsFileAssociationEntry &assoc) {
    return "Software\\Classes\\" + assoc.progId;
}

/// @brief Build the extension's hive-relative OpenWithProgids subkey.
/// @param assoc File association supplying the extension.
/// @return OpenWithProgids subkey path.
std::string openWithProgIdsKeyFor(const WindowsFileAssociationEntry &assoc) {
    return extensionKeyFor(assoc) + "\\OpenWithProgids";
}

/// @brief Emit RegOpenKeyW under a selected hive, closing the current open key first.
/// Jumps to missingLabel if the key does not exist (non-zero return).
/// @param gen Instruction emitter.
/// @param openSlot `RegOpenKeyW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param keyOff Embedded subkey path offset.
/// @param missingLabel Absent/open-failure target.
/// @param rootHkey Predefined registry root handle.
void emitRegOpenConstKeyIfExists(InstallerStubGen &gen,
                                 uint32_t openSlot,
                                 uint32_t closeSlot,
                                 uint32_t keyOff,
                                 uint32_t missingLabel,
                                 uint64_t rootHkey = kHkeyLocalMachine) {
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.movRegImm64(X64Reg::RCX, rootHkey);
    gen.leaRipData(X64Reg::RDX, keyOff);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kRegKeyOff);
    gen.callIATSlot(openSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(missingLabel);
}

/// @brief Emit conditional RegDeleteValueW: opens the key, deletes the named value, then closes.
/// Silently skips the entire sequence if the key does not exist.
/// @param gen Instruction emitter.
/// @param openSlot `RegOpenKeyW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param deleteValueSlot `RegDeleteValueW` slot.
/// @param keyOff Embedded subkey path offset.
/// @param valueNameOff Embedded value-name offset.
/// @param rootHkey Predefined registry root handle.
void emitRegDeleteNamedValueIfPresent(InstallerStubGen &gen,
                                      uint32_t openSlot,
                                      uint32_t closeSlot,
                                      uint32_t deleteValueSlot,
                                      uint32_t keyOff,
                                      uint32_t valueNameOff,
                                      uint64_t rootHkey = kHkeyLocalMachine) {
    const auto lblDone = gen.newLabel();
    emitRegOpenConstKeyIfExists(gen, openSlot, closeSlot, keyOff, lblDone, rootHkey);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.callIATSlot(deleteValueSlot);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.bindLabel(lblDone);
}

/// @brief Emit code to query a registry value and compare it to an embedded expected string.
/// Reads into kTempPathOff via RegQueryValueExW, checks the returned byte count,
/// then calls lstrcmpW. Jumps to notEqualLabel on query failure, size mismatch, or inequality.
/// @param gen Instruction emitter.
/// @param querySlot `RegQueryValueExW` slot.
/// @param strcmpSlot `lstrcmpW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param expectedValueOff Embedded expected UTF-16 value.
/// @param expectedBytes Expected serialized byte count.
/// @param notEqualLabel Query or comparison failure target.
void emitRegQueryConstStringEquals(InstallerStubGen &gen,
                                   uint32_t querySlot,
                                   uint32_t strcmpSlot,
                                   uint32_t valueNameOff,
                                   uint32_t expectedValueOff,
                                   uint32_t expectedBytes,
                                   uint32_t notEqualLabel) {
    zeroLocalQword(gen, kBytesReadOff);
    zeroLocalQword(gen, kBytesWrittenOff);
    gen.movMemImm32(X64Reg::RBP, kBytesReadOff, expectedBytes);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kBytesWrittenOff);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kTempPathOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(querySlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(notEqualLabel);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    emitCmpRegU32(gen, X64Reg::RAX, expectedBytes);
    gen.jnz(notEqualLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRipData(X64Reg::RDX, expectedValueOff);
    gen.callIATSlot(strcmpSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(notEqualLabel);
}

/// @brief Emit code to test whether a named registry value exists under kRegKeyOff.
/// Jumps to existsLabel if RegQueryValueExW returns success (0) or ERROR_MORE_DATA.
/// @param gen Instruction emitter.
/// @param querySlot `RegQueryValueExW` slot.
/// @param valueNameOff Embedded value-name offset.
/// @param existsLabel Present-value branch target.
void emitRegQueryValueExists(InstallerStubGen &gen,
                             uint32_t querySlot,
                             uint32_t valueNameOff,
                             uint32_t existsLabel) {
    zeroLocalQword(gen, kBytesReadOff);
    zeroLocalQword(gen, kBytesWrittenOff);
    gen.movMemImm32(X64Reg::RBP, kBytesReadOff, kMaxPathChars * 2u);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, valueNameOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kBytesWrittenOff);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kTempPathOff);
    gen.movMemReg(X64Reg::RSP, 0x20, X64Reg::RAX);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    gen.movMemReg(X64Reg::RSP, 0x28, X64Reg::RAX);
    gen.callIATSlot(querySlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(existsLabel);
    gen.cmpRegImm32(X64Reg::RAX, kErrorMoreData);
    gen.jz(existsLabel);
}

/// @brief Emit code to delete the ProgID subtree only when owned by this application.
/// Ownership is determined by comparing the ZAPSOwner registry value against the
/// embedded identifier string; deletes shell/open/command, shell/open, shell, and
/// the ProgID key itself. Silently skips if the key is absent or owned by another app.
/// @param gen Instruction emitter.
/// @param openSlot `RegOpenKeyW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param querySlot `RegQueryValueExW` slot.
/// @param strcmpSlot `lstrcmpW` slot.
/// @param deleteValueSlot `RegDeleteValueW` slot.
/// @param deleteSlot `RegDeleteTreeW` slot.
/// @param progKeyOff Embedded ProgID key.
/// @param markerNameOff Embedded ownership value name.
/// @param markerValueOff Embedded expected owner.
/// @param markerValueBytes Expected owner byte count.
/// @param commandKeyOff Embedded command subkey.
/// @param openKeyOff Embedded open subkey.
/// @param shellKeyOff Embedded shell subkey.
/// @param rootHkey Registry hive handle.
void emitDeleteProgIdTreeIfOwned(InstallerStubGen &gen,
                                 uint32_t openSlot,
                                 uint32_t closeSlot,
                                 uint32_t querySlot,
                                 uint32_t strcmpSlot,
                                 uint32_t deleteValueSlot,
                                 uint32_t deleteSlot,
                                 uint32_t progKeyOff,
                                 uint32_t markerNameOff,
                                 uint32_t markerValueOff,
                                 uint32_t markerValueBytes,
                                 uint32_t commandKeyOff,
                                 uint32_t openKeyOff,
                                 uint32_t shellKeyOff,
                                 uint64_t rootHkey = kHkeyLocalMachine) {
    const auto lblSkip = gen.newLabel();
    const auto lblNotOwned = gen.newLabel();
    emitRegOpenConstKeyIfExists(gen, openSlot, closeSlot, progKeyOff, lblSkip, rootHkey);
    emitRegQueryConstStringEquals(
        gen, querySlot, strcmpSlot, markerNameOff, markerValueOff, markerValueBytes, lblNotOwned);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
    gen.leaRipData(X64Reg::RDX, markerNameOff);
    gen.callIATSlot(deleteValueSlot);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    emitRegDeleteConstKey(gen, deleteSlot, commandKeyOff, rootHkey);
    emitRegDeleteConstKey(gen, deleteSlot, openKeyOff, rootHkey);
    emitRegDeleteConstKey(gen, deleteSlot, shellKeyOff, rootHkey);
    emitRegDeleteConstKey(gen, deleteSlot, progKeyOff, rootHkey);
    gen.jmp(lblSkip);
    gen.bindLabel(lblNotOwned);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.bindLabel(lblSkip);
}

/// @brief Emit code to register all file associations from layout.fileAssociations in the Windows
/// registry. For each association creates: Software\Classes\.<ext> with Content Type and
/// ZAPSContentTypeOwner, Software\Classes\.<ext>\OpenWithProgids\<ProgID> = REG_NONE,
/// Software\Classes\<ProgID> with ZAPSOwner marker and description,
/// and Software\Classes\<ProgID>\shell\open\command = "&lt;exe&gt;" [args] "%1".
/// @param gen Instruction emitter.
/// @param layout Association metadata and executable/icon paths.
/// @param slashOff Embedded path separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param createSlot `RegCreateKeyW` slot.
/// @param setValueSlot `RegSetValueExW` slot.
/// @param querySlot `RegQueryValueExW` slot.
/// @param strcmpSlot Reserved comparison slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param errorLabel Registry/path failure target.
void emitRegisterFileAssociations(InstallerStubGen &gen,
                                  const WindowsPackageLayout &layout,
                                  uint32_t slashOff,
                                  uint32_t copySlot,
                                  uint32_t catSlot,
                                  uint32_t createSlot,
                                  uint32_t setValueSlot,
                                  uint32_t querySlot,
                                  uint32_t strcmpSlot,
                                  uint32_t closeSlot,
                                  uint32_t strlenSlot,
                                  uint32_t errorLabel) {
    if (layout.fileAssociations.empty())
        return;
    (void)strcmpSlot;
    const uint64_t rootHkey = registryRootFor(layout);

    const uint32_t regContentTypeOff = gen.embedStringW("Content Type");
    const uint32_t regContentTypeOwnerOff = gen.embedStringW("ZAPSContentTypeOwner");
    const uint32_t regOwnerMarkerOff = gen.embedStringW("ZAPSOwner");
    const std::string ownerMarker =
        layout.identifier.empty() ? layout.displayName : layout.identifier;
    const uint32_t ownerMarkerBytes = wideBytesFor(ownerMarker);
    if (ownerMarkerBytes > kMaxPathChars * 2u)
        throw std::runtime_error("Windows registry owner marker is too long");
    const uint32_t ownerMarkerValueOff = gen.embedStringW(ownerMarker);
    const std::string associationExecutable =
        layout.fileAssociationExecutableRelativePath.empty()
            ? layout.executableName
            : windowsPathFragment(layout.fileAssociationExecutableRelativePath);
    const uint32_t exeNameOff = gen.embedStringW(associationExecutable);
    const std::string associationIcon = layout.displayIconRelativePath.empty()
                                            ? associationExecutable
                                            : windowsPathFragment(layout.displayIconRelativePath);
    const uint32_t iconNameOff = gen.embedStringW(associationIcon);
    const uint32_t quoteOff = gen.embedStringW("\"");
    const uint32_t quotedFileArgOff = gen.embedStringW(" \"%1\"");

    for (const auto &assoc : layout.fileAssociations) {
        const uint32_t commandArgsOff = assoc.openCommandArguments.empty()
                                            ? 0
                                            : gen.embedStringW(" " + assoc.openCommandArguments);
        const uint32_t extKeyOff = gen.embedStringW(extensionKeyFor(assoc));
        const uint32_t progIdOff = gen.embedStringW(assoc.progId);
        emitRegCreateConstKey(gen, createSlot, extKeyOff, errorLabel, rootHkey);
        if (!assoc.mimeType.empty()) {
            const auto lblContentTypeExists = gen.newLabel();
            const auto lblContentTypeDone = gen.newLabel();
            const uint32_t mimeOff = gen.embedStringW(assoc.mimeType);
            emitRegQueryValueExists(gen, querySlot, regContentTypeOff, lblContentTypeExists);
            emitRegSetConstString(gen,
                                  setValueSlot,
                                  regContentTypeOff,
                                  mimeOff,
                                  wideBytesFor(assoc.mimeType),
                                  errorLabel);
            emitRegSetConstString(gen,
                                  setValueSlot,
                                  regContentTypeOwnerOff,
                                  ownerMarkerValueOff,
                                  ownerMarkerBytes,
                                  errorLabel);
            gen.jmp(lblContentTypeDone);
            gen.bindLabel(lblContentTypeExists);
            gen.bindLabel(lblContentTypeDone);
        }
        emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);

        const uint32_t openWithKeyOff = gen.embedStringW(openWithProgIdsKeyFor(assoc));
        emitRegCreateConstKey(gen, createSlot, openWithKeyOff, errorLabel, rootHkey);
        emitRegSetConstNoneValue(gen, setValueSlot, progIdOff, errorLabel);
        emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);

        const uint32_t progKeyOff = gen.embedStringW(progIdKeyFor(assoc));
        const std::string description =
            assoc.description.empty() ? layout.displayName : assoc.description;
        const uint32_t descriptionOff = gen.embedStringW(description);
        emitRegCreateConstKey(gen, createSlot, progKeyOff, errorLabel, rootHkey);
        emitRegSetConstString(gen,
                              setValueSlot,
                              regOwnerMarkerOff,
                              ownerMarkerValueOff,
                              ownerMarkerBytes,
                              errorLabel);
        emitRegSetDefaultConstString(
            gen, setValueSlot, descriptionOff, wideBytesFor(description), errorLabel);
        emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);

        const uint32_t commandKeyOff =
            gen.embedStringW(progIdKeyFor(assoc) + "\\shell\\open\\command");
        const uint32_t defaultIconKeyOff = gen.embedStringW(progIdKeyFor(assoc) + "\\DefaultIcon");
        emitRegCreateConstKey(gen, createSlot, defaultIconKeyOff, errorLabel, rootHkey);
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
        gen.leaRipData(X64Reg::RDX, quoteOff);
        gen.callIATSlot(copySlot);
        emitCheckedCatStack(gen, kTempPathOff, kInstallPathOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, slashOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, iconNameOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, quoteOff, catSlot, strlenSlot, errorLabel);
        emitRegSetDefaultStackString(gen, setValueSlot, strlenSlot, kTempPathOff, errorLabel);
        emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);

        emitRegCreateConstKey(gen, createSlot, commandKeyOff, errorLabel, rootHkey);
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
        gen.leaRipData(X64Reg::RDX, quoteOff);
        gen.callIATSlot(copySlot);
        emitCheckedCatStack(gen, kTempPathOff, kInstallPathOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, slashOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, exeNameOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kTempPathOff, quoteOff, catSlot, strlenSlot, errorLabel);
        if (!assoc.openCommandArguments.empty())
            emitCheckedCatEmbedded(
                gen, kTempPathOff, commandArgsOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(
            gen, kTempPathOff, quotedFileArgOff, catSlot, strlenSlot, errorLabel);
        emitRegSetDefaultStackString(gen, setValueSlot, strlenSlot, kTempPathOff, errorLabel);
        emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    }
}

/// @brief Emit code to unregister all file associations owned by this application.
/// Removes OpenWithProgids entries, Content Type if ZAPSContentTypeOwner matches,
/// and the full ProgID subtree if the ZAPSOwner marker matches our identifier.
/// Silently skips any association whose key is missing or owned by another app.
/// @param gen Instruction emitter.
/// @param layout Associations and package owner identity.
/// @param openSlot `RegOpenKeyW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param querySlot `RegQueryValueExW` slot.
/// @param strcmpSlot `lstrcmpW` slot.
/// @param deleteValueSlot `RegDeleteValueW` slot.
/// @param deleteSlot `RegDeleteTreeW` slot.
void emitUnregisterFileAssociations(InstallerStubGen &gen,
                                    const WindowsPackageLayout &layout,
                                    uint32_t openSlot,
                                    uint32_t closeSlot,
                                    uint32_t querySlot,
                                    uint32_t strcmpSlot,
                                    uint32_t deleteValueSlot,
                                    uint32_t deleteSlot) {
    const uint64_t rootHkey = registryRootFor(layout);
    const uint32_t ownerMarkerOff = gen.embedStringW("ZAPSOwner");
    const uint32_t contentTypeOwnerOff = gen.embedStringW("ZAPSContentTypeOwner");
    const std::string ownerMarker =
        layout.identifier.empty() ? layout.displayName : layout.identifier;
    const uint32_t ownerMarkerValueOff = gen.embedStringW(ownerMarker);
    const uint32_t ownerMarkerValueBytes = wideBytesFor(ownerMarker);
    if (ownerMarkerValueBytes > kMaxPathChars * 2u)
        throw std::runtime_error("Windows registry owner marker is too long");
    for (const auto &assoc : layout.fileAssociations) {
        emitRegDeleteNamedValueIfPresent(gen,
                                         openSlot,
                                         closeSlot,
                                         deleteValueSlot,
                                         gen.embedStringW(openWithProgIdsKeyFor(assoc)),
                                         gen.embedStringW(assoc.progId),
                                         rootHkey);
        if (!assoc.mimeType.empty()) {
            const auto lblContentDone = gen.newLabel();
            const auto lblContentNotOwned = gen.newLabel();
            emitRegOpenConstKeyIfExists(gen,
                                        openSlot,
                                        closeSlot,
                                        gen.embedStringW(extensionKeyFor(assoc)),
                                        lblContentDone,
                                        rootHkey);
            emitRegQueryConstStringEquals(gen,
                                          querySlot,
                                          strcmpSlot,
                                          contentTypeOwnerOff,
                                          ownerMarkerValueOff,
                                          ownerMarkerValueBytes,
                                          lblContentNotOwned);
            gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
            gen.leaRipData(X64Reg::RDX, gen.embedStringW("Content Type"));
            gen.callIATSlot(deleteValueSlot);
            gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kRegKeyOff);
            gen.leaRipData(X64Reg::RDX, contentTypeOwnerOff);
            gen.callIATSlot(deleteValueSlot);
            emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
            gen.jmp(lblContentDone);
            gen.bindLabel(lblContentNotOwned);
            emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
            gen.bindLabel(lblContentDone);
        }
        emitRegDeleteConstKey(
            gen, deleteSlot, gen.embedStringW(progIdKeyFor(assoc) + "\\DefaultIcon"), rootHkey);
        emitDeleteProgIdTreeIfOwned(
            gen,
            openSlot,
            closeSlot,
            querySlot,
            strcmpSlot,
            deleteValueSlot,
            deleteSlot,
            gen.embedStringW(progIdKeyFor(assoc)),
            ownerMarkerOff,
            ownerMarkerValueOff,
            ownerMarkerValueBytes,
            gen.embedStringW(progIdKeyFor(assoc) + "\\shell\\open\\command"),
            gen.embedStringW(progIdKeyFor(assoc) + "\\shell\\open"),
            gen.embedStringW(progIdKeyFor(assoc) + "\\shell"),
            rootHkey);
    }
}

/// @brief Emit SHGetKnownFolderPath into a stack WCHAR buffer and free the shell string.
/// @param gen Instruction emitter.
/// @param folderId Embedded KNOWNFOLDERID offset.
/// @param destOff Destination frame-buffer offset.
/// @param copySlot `lstrcpyW` slot.
/// @param folderSlot `SHGetKnownFolderPath` slot.
/// @param freeSlot `CoTaskMemFree` slot.
/// @param errorLabel Resolution failure target.
void emitResolveKnownFolderPath(InstallerStubGen &gen,
                                const KnownFolderGuid &folderId,
                                int32_t destOff,
                                uint32_t copySlot,
                                uint32_t folderSlot,
                                uint32_t freeSlot,
                                uint32_t errorLabel) {
    const uint32_t folderIdOff = gen.embedBytes(folderId.data(), folderId.size());
    zeroLocalQword(gen, kKnownFolderPtrOff);
    gen.leaRipData(X64Reg::RCX, folderIdOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kKnownFolderPtrOff);
    gen.callIATSlot(folderSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kKnownFolderPtrOff);
    gen.callIATSlot(copySlot);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kKnownFolderPtrOff);
    gen.callIATSlot(freeSlot);
    zeroLocalQword(gen, kKnownFolderPtrOff);
}

/// @brief Emit code to resolve all required install root paths via SHGetKnownFolderPath.
/// Populates kInstallPathOff (always), kDesktopPathOff (if needsDesktopPath), and
/// kMenuPathOff (if needsMenuPath); appends the install-dir subdirectory to kInstallPathOff.
/// If `createRoots` is true, also emits CreateDirectoryW for each resolved root.
/// @param gen Instruction emitter.
/// @param layout Scope and required-root metadata.
/// @param slashOff Embedded separator.
/// @param installDirOff Embedded install-directory leaf.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param createDirSlot `CreateDirectoryW` slot.
/// @param getLastErrorSlot `GetLastError` slot.
/// @param folderSlot `SHGetKnownFolderPath` slot.
/// @param freeSlot `CoTaskMemFree` slot.
/// @param createRoots Whether to create resolved directories.
/// @param errorLabel Failure target.
void emitBuildRootPaths(InstallerStubGen &gen,
                        const WindowsPackageLayout &layout,
                        uint32_t slashOff,
                        uint32_t installDirOff,
                        uint32_t copySlot,
                        uint32_t catSlot,
                        uint32_t strlenSlot,
                        uint32_t createDirSlot,
                        uint32_t getLastErrorSlot,
                        uint32_t folderSlot,
                        uint32_t freeSlot,
                        bool createRoots,
                        uint32_t errorLabel) {
    emitResolveKnownFolderPath(gen,
                               installRootFolderIdFor(layout),
                               kInstallPathOff,
                               copySlot,
                               folderSlot,
                               freeSlot,
                               errorLabel);

    emitCheckedCatEmbedded(gen, kInstallPathOff, slashOff, catSlot, strlenSlot, errorLabel);
    emitCheckedCatEmbedded(gen, kInstallPathOff, installDirOff, catSlot, strlenSlot, errorLabel);
    if (createRoots) {
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kInstallPathOff);
        gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
        emitCreateDirectoryChecked(gen, createDirSlot, getLastErrorSlot, errorLabel);
    }

    if (needsDesktopPath(layout)) {
        emitResolveKnownFolderPath(gen,
                                   desktopFolderIdFor(layout),
                                   kDesktopPathOff,
                                   copySlot,
                                   folderSlot,
                                   freeSlot,
                                   errorLabel);
    }

    if (needsMenuPath(layout)) {
        emitResolveKnownFolderPath(gen,
                                   programsFolderIdFor(layout),
                                   kMenuPathOff,
                                   copySlot,
                                   folderSlot,
                                   freeSlot,
                                   errorLabel);

        emitCheckedCatEmbedded(gen, kMenuPathOff, slashOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen, kMenuPathOff, installDirOff, catSlot, strlenSlot, errorLabel);
        if (createRoots) {
            gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMenuPathOff);
            gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
            emitCreateDirectoryChecked(gen, createDirSlot, getLastErrorSlot, errorLabel);
        }
    }
}

/// @brief Emit code to compose the system PATH entry string into [RBP+destOff].
/// Copies kInstallPathOff, then appends "\<pathRelativePath>" when configured.
/// @param gen Instruction emitter.
/// @param layout Optional PATH-relative subdirectory.
/// @param destOff Destination frame-buffer offset.
/// @param slashOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param errorLabel Length failure target.
void emitComposeInstallPathEntry(InstallerStubGen &gen,
                                 const WindowsPackageLayout &layout,
                                 int32_t destOff,
                                 uint32_t slashOff,
                                 uint32_t copySlot,
                                 uint32_t catSlot,
                                 uint32_t strlenSlot,
                                 uint32_t errorLabel) {
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, destOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kInstallPathOff);
    gen.callIATSlot(copySlot);
    if (!layout.pathRelativePath.empty()) {
        emitCheckedCatEmbedded(gen, destOff, slashOff, catSlot, strlenSlot, errorLabel);
        emitCheckedCatEmbedded(gen,
                               destOff,
                               gen.embedStringW(windowsPathFragment(layout.pathRelativePath)),
                               catSlot,
                               strlenSlot,
                               errorLabel);
    }
}

/// @brief Emit code to delete all contents of the install root before extraction.
/// Builds an "<installDir>\*" double-null-terminated glob, fills an SHFILEOPSTRUCT
/// with FO_DELETE | FOF_NOCONFIRMATION | FOF_NOERRORUI, and calls SHFileOperationW.
/// No-op when layout.cleanInstallRootBeforeInstall is false.
/// @param gen Instruction emitter.
/// @param layout Clean-install policy.
/// @param slashOff Embedded separator.
/// @param starOff Embedded wildcard.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param fileOperationSlot `SHFileOperationW` slot.
/// @param errorLabel Failure target.
void emitCleanInstallRootContents(InstallerStubGen &gen,
                                  const WindowsPackageLayout &layout,
                                  uint32_t slashOff,
                                  uint32_t starOff,
                                  uint32_t copySlot,
                                  uint32_t catSlot,
                                  uint32_t strlenSlot,
                                  uint32_t fileOperationSlot,
                                  uint32_t errorLabel) {
    if (!layout.cleanInstallRootBeforeInstall)
        return;

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kPathExpectedOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kInstallPathOff);
    gen.callIATSlot(copySlot);
    emitCheckedCatEmbedded(gen, kPathExpectedOff, slashOff, catSlot, strlenSlot, errorLabel);
    emitCheckedCatEmbedded(gen, kPathExpectedOff, starOff, catSlot, strlenSlot, errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kPathExpectedOff);
    gen.callIATSlot(strlenSlot);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 2);
    gen.ja(errorLabel);
    gen.movMemIndexImm16(X64Reg::RBP, X64Reg::RAX, 1, kPathExpectedOff + 2, 0);

    for (int32_t off = 0; off < 56; off += 8)
        zeroLocalQword(gen, kFileOpStructOff + off);
    gen.movMemImm32(X64Reg::RBP, kFileOpStructOff + 8, kFileOperationDelete);
    gen.leaRegMem(X64Reg::RAX, X64Reg::RBP, kPathExpectedOff);
    gen.movMemReg(X64Reg::RBP, kFileOpStructOff + 16, X64Reg::RAX);
    gen.movMemImm32(X64Reg::RBP, kFileOpStructOff + 32, kFileOperationSilentFlags);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kFileOpStructOff);
    gen.callIATSlot(fileOperationSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kFileOpStructOff + 36);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(errorLabel);
}

// Forward declaration — implemented after emitRestorePathFromOriginalLocal
// to allow emitInstallPathUpdate to reference it.
/// @brief Emit rebuilding of PATH while omitting one case-insensitive token.
/// @param gen Instruction emitter.
/// @param currentPathOff Source PATH frame buffer.
/// @param entryOff Token to remove.
/// @param outputPathOff Destination PATH frame buffer.
/// @param semicolonOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strcmpSlot `lstrcmpiW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param errorLabel Failure target.
void emitRemovePathEntryTokens(InstallerStubGen &gen,
                               int32_t currentPathOff,
                               int32_t entryOff,
                               int32_t outputPathOff,
                               uint32_t semicolonOff,
                               uint32_t copySlot,
                               uint32_t catSlot,
                               uint32_t strcmpSlot,
                               uint32_t strlenSlot,
                               uint32_t errorLabel);

/// @brief Emit code to add the install path entry to the system PATH registry value.
/// Idempotent: checks ZAPSPathEntry in the uninstall key first and skips if already present.
/// Otherwise reads the current PATH, strips any stale entry via emitRemovePathEntryTokens,
/// appends the new entry, writes back as REG_EXPAND_SZ, and broadcasts WM_SETTINGCHANGE.
/// No-op when layout.addToPath is false.
/// @param gen Instruction emitter.
/// @param layout PATH policy and installation scope.
/// @param slashOff Embedded path separator.
/// @param semicolonOff Embedded PATH separator.
/// @param uninstallKeyOff Embedded uninstall subkey.
/// @param envKeyOff Embedded environment subkey.
/// @param regPathValueOff Embedded `Path` value name.
/// @param regOriginalPathOff Embedded rollback value name.
/// @param regPathEntryOff Embedded owned-entry value name.
/// @param environmentOff Embedded broadcast text.
/// @param createSlot `RegCreateKeyW` slot.
/// @param openSlot `RegOpenKeyW` slot.
/// @param querySlot `RegQueryValueExW` slot.
/// @param setValueSlot `RegSetValueExW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strcmpSlot `lstrcmpiW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param sendSlot `SendMessageTimeoutW` slot.
/// @param errorLabel Failure target.
void emitInstallPathUpdate(InstallerStubGen &gen,
                           const WindowsPackageLayout &layout,
                           uint32_t slashOff,
                           uint32_t semicolonOff,
                           uint32_t uninstallKeyOff,
                           uint32_t envKeyOff,
                           uint32_t regPathValueOff,
                           uint32_t regOriginalPathOff,
                           uint32_t regPathEntryOff,
                           uint32_t environmentOff,
                           uint32_t createSlot,
                           uint32_t openSlot,
                           uint32_t querySlot,
                           uint32_t setValueSlot,
                           uint32_t closeSlot,
                           uint32_t copySlot,
                           uint32_t catSlot,
                           uint32_t strcmpSlot,
                           uint32_t strlenSlot,
                           uint32_t sendSlot,
                           uint32_t errorLabel) {
    if (!layout.addToPath)
        return;
    (void)regOriginalPathOff;
    const uint64_t rootHkey = registryRootFor(layout);

    const auto lblDoAppend = gen.newLabel();
    const auto lblPathMissing = gen.newLabel();
    const auto lblCheckCurrentPath = gen.newLabel();
    const auto lblUseCleanedPath = gen.newLabel();
    const auto lblWritePath = gen.newLabel();
    const auto lblSkipUpdateClose = gen.newLabel();
    const auto lblSkipUpdate = gen.newLabel();

    emitComposeInstallPathEntry(
        gen, layout, kUninstallPathOff, slashOff, copySlot, catSlot, strlenSlot, errorLabel);

    emitRegOpenConstKeyIfExists(
        gen, openSlot, closeSlot, uninstallKeyOff, lblCheckCurrentPath, rootHkey);
    emitRegQueryStackString(gen,
                            querySlot,
                            regPathEntryOff,
                            kTempPathOff,
                            kMaxPathChars * 2u,
                            lblCheckCurrentPath,
                            lblCheckCurrentPath);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kUninstallPathOff);
    gen.callIATSlot(strcmpSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblCheckCurrentPath);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.jmp(lblSkipUpdate);

    gen.bindLabel(lblCheckCurrentPath);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    emitRegOpenConstKeyIfExists(gen, openSlot, closeSlot, envKeyOff, lblDoAppend, rootHkey);
    emitRegQueryStackString(gen,
                            querySlot,
                            regPathValueOff,
                            kPathOriginalOff,
                            kMaxPathChars * 2u,
                            lblDoAppend,
                            lblSkipUpdateClose);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kPathOriginalOff);
    gen.callIATSlot(copySlot);
    emitRemovePathEntryTokens(gen,
                              kTempPathOff,
                              kUninstallPathOff,
                              kPathExpectedOff,
                              semicolonOff,
                              copySlot,
                              catSlot,
                              strcmpSlot,
                              strlenSlot,
                              errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kPathExpectedOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kPathOriginalOff);
    gen.callIATSlot(strcmpSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblUseCleanedPath);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.jmp(lblDoAppend);

    gen.bindLabel(lblUseCleanedPath);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kPathExpectedOff);
    gen.callIATSlot(copySlot);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    emitRegCreateConstKey(gen, createSlot, envKeyOff, errorLabel, rootHkey);
    gen.jmp(lblWritePath);

    gen.bindLabel(lblDoAppend);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    emitRegCreateConstKey(gen, createSlot, envKeyOff, errorLabel, rootHkey);
    gen.movMemImm32(X64Reg::RBP, kPathOriginalOff, 0);
    emitRegQueryStackString(gen,
                            querySlot,
                            regPathValueOff,
                            kPathOriginalOff,
                            kMaxPathChars * 2u,
                            lblPathMissing,
                            lblSkipUpdateClose);
    gen.bindLabel(lblPathMissing);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kPathOriginalOff);
    gen.callIATSlot(copySlot);
    gen.bindLabel(lblWritePath);
    emitAppendSeparatorIfNonEmpty(gen, kTempPathOff, semicolonOff, catSlot, strlenSlot, errorLabel);
    emitCheckedCatStack(gen, kTempPathOff, kUninstallPathOff, catSlot, strlenSlot, errorLabel);

    emitRegSetStackString(
        gen, setValueSlot, strlenSlot, regPathValueOff, kTempPathOff, kRegExpandSz, errorLabel);
    emitBroadcastEnvironmentChange(gen, sendSlot, environmentOff);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.movRegImm32(X64Reg::RAX, 1);
    gen.movMemReg(X64Reg::RBP, kPathUpdatedOff, X64Reg::RAX);
    gen.jmp(lblSkipUpdate);
    gen.bindLabel(lblSkipUpdateClose);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.bindLabel(lblSkipUpdate);
}

/// @brief Emit rollback code to restore the system PATH from kPathOriginalOff.
/// Only executes if kPathUpdatedOff is non-zero, meaning PATH was actually modified
/// earlier in the install sequence. No-op when layout.addToPath is false.
/// @param gen Instruction emitter.
/// @param layout PATH policy and scope.
/// @param envKeyOff Embedded environment key.
/// @param regPathValueOff Embedded `Path` name.
/// @param environmentOff Embedded broadcast text.
/// @param createSlot `RegCreateKeyW` slot.
/// @param setValueSlot `RegSetValueExW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param sendSlot `SendMessageTimeoutW` slot.
void emitRestorePathFromOriginalLocal(InstallerStubGen &gen,
                                      const WindowsPackageLayout &layout,
                                      uint32_t envKeyOff,
                                      uint32_t regPathValueOff,
                                      uint32_t environmentOff,
                                      uint32_t createSlot,
                                      uint32_t setValueSlot,
                                      uint32_t closeSlot,
                                      uint32_t strlenSlot,
                                      uint32_t sendSlot) {
    if (!layout.addToPath)
        return;

    const uint64_t rootHkey = registryRootFor(layout);
    const auto lblSkip = gen.newLabel();
    const auto lblClose = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kPathUpdatedOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblSkip);
    emitRegCreateConstKey(gen, createSlot, envKeyOff, lblSkip, rootHkey);
    emitRegSetStackString(
        gen, setValueSlot, strlenSlot, regPathValueOff, kPathOriginalOff, kRegExpandSz, lblClose);
    emitBroadcastEnvironmentChange(gen, sendSlot, environmentOff);
    gen.bindLabel(lblClose);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.bindLabel(lblSkip);
}

/// @brief Emit code to rebuild the PATH string at [RBP+outputPathOff] without our entry.
/// Iterates the semicolon-delimited tokens in [RBP+currentPathOff], copying each token to
/// the output except those that match [RBP+entryOff] (case-insensitive via lstrcmpiW).
/// Used on install (remove stale entry before re-appending) and uninstall (remove our entry).
/// @param gen Instruction emitter.
/// @param currentPathOff Source PATH buffer.
/// @param entryOff Token to remove.
/// @param outputPathOff Destination buffer.
/// @param semicolonOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strcmpSlot `lstrcmpiW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param errorLabel Length/parser failure target.
void emitRemovePathEntryTokens(InstallerStubGen &gen,
                               int32_t currentPathOff,
                               int32_t entryOff,
                               int32_t outputPathOff,
                               uint32_t semicolonOff,
                               uint32_t copySlot,
                               uint32_t catSlot,
                               uint32_t strcmpSlot,
                               uint32_t strlenSlot,
                               uint32_t errorLabel) {
    const auto lblLoop = gen.newLabel();
    const auto lblFindEnd = gen.newLabel();
    const auto lblTokenSeparator = gen.newLabel();
    const auto lblTokenEnd = gen.newLabel();
    const auto lblProcessToken = gen.newLabel();
    const auto lblAppendToken = gen.newLabel();
    const auto lblSkipToken = gen.newLabel();
    const auto lblContinue = gen.newLabel();
    const auto lblDone = gen.newLabel();

    (void)copySlot;
    gen.movMemImm32(X64Reg::RBP, outputPathOff, 0);

    gen.xorRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.bindLabel(lblLoop);
    gen.movMemReg(X64Reg::RBP, kCurrentFileOffsetOff, X64Reg::RAX);

    gen.bindLabel(lblFindEnd);
    gen.movzxRegMemIndex16(X64Reg::R11, X64Reg::RBP, X64Reg::RAX, 0, currentPathOff);
    gen.cmpRegImm32(X64Reg::R11, ';');
    gen.jz(lblTokenSeparator);
    gen.testRegReg(X64Reg::R11, X64Reg::R11);
    gen.jz(lblTokenEnd);
    gen.addRegImm32(X64Reg::RAX, 2);
    gen.jmp(lblFindEnd);

    gen.bindLabel(lblTokenSeparator);
    gen.movMemReg(X64Reg::RBP, kRemainingBytesOff, X64Reg::RAX);
    gen.movMemIndexImm16(X64Reg::RBP, X64Reg::RAX, 0, currentPathOff, 0);
    gen.addRegImm32(X64Reg::RAX, 2);
    gen.movMemReg(X64Reg::RBP, kCurrentChunkBytesOff, X64Reg::RAX);
    gen.jmp(lblProcessToken);

    gen.bindLabel(lblTokenEnd);
    gen.movMemReg(X64Reg::RBP, kRemainingBytesOff, X64Reg::RAX);
    gen.movMemReg(X64Reg::RBP, kCurrentChunkBytesOff, X64Reg::RAX);

    gen.bindLabel(lblProcessToken);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentFileOffsetOff);
    gen.movRegMem(X64Reg::R11, X64Reg::RBP, kRemainingBytesOff);
    gen.cmpRegReg(X64Reg::R10, X64Reg::R11);
    gen.jz(lblSkipToken);
    gen.leaRegMemIndex(X64Reg::RCX, X64Reg::RBP, X64Reg::R10, 0, currentPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, entryOff);
    gen.callIATSlot(strcmpSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblSkipToken);

    gen.bindLabel(lblAppendToken);
    emitAppendSeparatorIfNonEmpty(
        gen, outputPathOff, semicolonOff, catSlot, strlenSlot, errorLabel);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentFileOffsetOff);
    gen.leaRegMemIndex(X64Reg::RDX, X64Reg::RBP, X64Reg::R10, 0, currentPathOff);
    emitCheckedCatReg(gen, outputPathOff, X64Reg::RDX, catSlot, strlenSlot, errorLabel);

    gen.bindLabel(lblSkipToken);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.movzxRegMemIndex16(X64Reg::R11, X64Reg::RBP, X64Reg::RAX, 0, currentPathOff);
    gen.testRegReg(X64Reg::R11, X64Reg::R11);
    gen.jz(lblDone);
    gen.jmp(lblContinue);

    gen.bindLabel(lblContinue);
    gen.jmp(lblLoop);

    gen.bindLabel(lblDone);
}

/// @brief Emit code to remove our PATH entry during uninstall.
/// Reads ZAPSPathEntry from the uninstall registry key to recover the exact entry
/// string that was added, strips it from the current system PATH via
/// emitRemovePathEntryTokens, writes the cleaned PATH back as REG_EXPAND_SZ,
/// and broadcasts WM_SETTINGCHANGE. No-op when layout.addToPath is false or
/// the uninstall key does not exist.
/// @param gen Instruction emitter.
/// @param layout PATH policy and installation scope.
/// @param slashOff Embedded path separator.
/// @param semicolonOff Embedded PATH separator.
/// @param uninstallKeyOff Embedded uninstall key.
/// @param envKeyOff Embedded environment key.
/// @param regPathValueOff Embedded `Path` name.
/// @param regPathEntryOff Embedded owned-entry name.
/// @param environmentOff Embedded broadcast text.
/// @param openSlot `RegOpenKeyW` slot.
/// @param createSlot `RegCreateKeyW` slot.
/// @param setValueSlot `RegSetValueExW` slot.
/// @param querySlot `RegQueryValueExW` slot.
/// @param closeSlot `RegCloseKey` slot.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strcmpSlot `lstrcmpiW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param sendSlot `SendMessageTimeoutW` slot.
/// @param errorLabel Failure target.
void emitRestorePathFromUninstallKey(InstallerStubGen &gen,
                                     const WindowsPackageLayout &layout,
                                     uint32_t slashOff,
                                     uint32_t semicolonOff,
                                     uint32_t uninstallKeyOff,
                                     uint32_t envKeyOff,
                                     uint32_t regPathValueOff,
                                     uint32_t regPathEntryOff,
                                     uint32_t environmentOff,
                                     uint32_t openSlot,
                                     uint32_t createSlot,
                                     uint32_t setValueSlot,
                                     uint32_t querySlot,
                                     uint32_t closeSlot,
                                     uint32_t copySlot,
                                     uint32_t catSlot,
                                     uint32_t strcmpSlot,
                                     uint32_t strlenSlot,
                                     uint32_t sendSlot,
                                     uint32_t errorLabel) {
    if (!layout.addToPath)
        return;

    const uint64_t rootHkey = registryRootFor(layout);
    const auto lblSkip = gen.newLabel();
    const auto lblEntryMissing = gen.newLabel();
    const auto lblEnvMissing = gen.newLabel();

    emitRegOpenConstKeyIfExists(gen, openSlot, closeSlot, uninstallKeyOff, lblSkip, rootHkey);
    emitRegQueryStackString(gen,
                            querySlot,
                            regPathEntryOff,
                            kUninstallPathOff,
                            kMaxPathChars * 2u,
                            lblEntryMissing,
                            lblEntryMissing);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);

    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.movRegImm64(X64Reg::RCX, rootHkey);
    gen.leaRipData(X64Reg::RDX, envKeyOff);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kRegKeyOff);
    gen.callIATSlot(createSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblEnvMissing);
    emitRegQueryStackString(gen,
                            querySlot,
                            regPathValueOff,
                            kTempPathOff,
                            kMaxPathChars * 2u,
                            lblEnvMissing,
                            lblEnvMissing);
    emitRemovePathEntryTokens(gen,
                              kTempPathOff,
                              kUninstallPathOff,
                              kPathExpectedOff,
                              semicolonOff,
                              copySlot,
                              catSlot,
                              strcmpSlot,
                              strlenSlot,
                              errorLabel);
    emitRegSetStackString(gen,
                          setValueSlot,
                          strlenSlot,
                          regPathValueOff,
                          kPathExpectedOff,
                          kRegExpandSz,
                          lblEnvMissing);
    emitBroadcastEnvironmentChange(gen, sendSlot, environmentOff);
    gen.bindLabel(lblEnvMissing);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.jmp(lblSkip);

    gen.bindLabel(lblEntryMissing);
    emitRegCloseIfSet(gen, kRegKeyOff, closeSlot);
    gen.bindLabel(lblSkip);
}

/// @brief Emit code to extract a single file from the ZIP overlay appended to the installer PE.
/// Seeks to `overlayFileOffset + entry.overlayDataOffset` via SetFilePointerEx, reads in
/// kInstallerCopyChunkBytes chunks with incremental CRC-32 via RtlComputeCrc32, creates
/// the destination file, writes each chunk, and verifies the final CRC against entry.crc32.
/// Zero-byte files skip the read/CRC loop and only create the destination file.
/// @param gen Instruction emitter.
/// @param entry File destination, stored offset, length, and CRC metadata.
/// @param overlayFileOffset Absolute PE offset where the ZIP overlay begins.
/// @param slashOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param createFileSlot `CreateFileW` slot.
/// @param readSlot `ReadFile` slot.
/// @param writeSlot `WriteFile` slot.
/// @param closeSlot `CloseHandle` slot.
/// @param allocSlot `LocalAlloc` slot.
/// @param freeSlot `LocalFree` slot.
/// @param setFilePointerSlot `SetFilePointerEx` slot.
/// @param crcSlot `RtlComputeCrc32` slot.
/// @param errorLabel I/O, size, or checksum failure target.
void emitExtractFile(InstallerStubGen &gen,
                     const WindowsPackageFileEntry &entry,
                     uint64_t overlayFileOffset,
                     uint32_t slashOff,
                     uint32_t copySlot,
                     uint32_t catSlot,
                     uint32_t strlenSlot,
                     uint32_t createFileSlot,
                     uint32_t readSlot,
                     uint32_t writeSlot,
                     uint32_t closeSlot,
                     uint32_t allocSlot,
                     uint32_t freeSlot,
                     uint32_t setFilePointerSlot,
                     uint32_t crcSlot,
                     uint32_t errorLabel) {
    if (entry.sizeBytes > UINT32_MAX)
        throw std::runtime_error("Windows installer entry is too large: " + entry.relativePath);
    const uint32_t entrySize = static_cast<uint32_t>(entry.sizeBytes);
    const uint64_t entryOffset = overlayFileOffset + entry.overlayDataOffset;
    if (entryOffset < overlayFileOffset)
        throw std::runtime_error("Windows installer overlay offset overflow: " +
                                 entry.relativePath);
    const uint32_t relPathOff = gen.embedStringW(entry.relativePath);

    if (entrySize == 0) {
        emitComposePath(gen,
                        entry.root,
                        kTempPathOff,
                        slashOff,
                        relPathOff,
                        copySlot,
                        catSlot,
                        strlenSlot,
                        errorLabel);

        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
        gen.movRegImm32(X64Reg::RDX, kGenericWrite);
        gen.xorRegReg(X64Reg::R8, X64Reg::R8);
        gen.xorRegReg(X64Reg::R9, X64Reg::R9);
        storeStackImm64(gen, 0x20, kCreateAlways);
        storeStackImm64(gen, 0x28, kFileAttributeNormal);
        storeStackImm64(gen, 0x30, 0);
        gen.callIATSlot(createFileSlot);
        gen.cmpRegImm32(X64Reg::RAX, 0xFFFFFFFFu);
        gen.jz(errorLabel);
        gen.movMemReg(X64Reg::RBP, kHOutOff, X64Reg::RAX);
        emitCloseLocalHandleIfSet(gen, kHOutOff, closeSlot);
        return;
    }

    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.movRegImm32(X64Reg::RDX, std::min(entrySize, kInstallerCopyChunkBytes));
    gen.callIATSlot(allocSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movMemReg(X64Reg::RBP, kPFileBufOff, X64Reg::RAX);

    emitComposePath(gen,
                    entry.root,
                    kTempPathOff,
                    slashOff,
                    relPathOff,
                    copySlot,
                    catSlot,
                    strlenSlot,
                    errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.movRegImm32(X64Reg::RDX, kGenericWrite);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    storeStackImm64(gen, 0x20, kCreateAlways);
    storeStackImm64(gen, 0x28, kFileAttributeNormal);
    storeStackImm64(gen, 0x30, 0);
    gen.callIATSlot(createFileSlot);
    gen.cmpRegImm32(X64Reg::RAX, 0xFFFFFFFFu);
    gen.jz(errorLabel);
    gen.movMemReg(X64Reg::RBP, kHOutOff, X64Reg::RAX);

    gen.movRegImm32(X64Reg::RAX, entrySize);
    gen.movMemReg(X64Reg::RBP, kRemainingBytesOff, X64Reg::RAX);
    gen.movRegImm64(X64Reg::RAX, entryOffset);
    gen.movMemReg(X64Reg::RBP, kCurrentFileOffsetOff, X64Reg::RAX);
    zeroLocalQword(gen, kCrcOff);

    const auto lblLoop = gen.newLabel();
    const auto lblUseMaxChunk = gen.newLabel();
    const auto lblHaveChunk = gen.newLabel();
    const auto lblDone = gen.newLabel();

    gen.bindLabel(lblLoop);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kRemainingBytesOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblDone);
    gen.cmpRegImm32(X64Reg::RAX, kInstallerCopyChunkBytes);
    gen.ja(lblUseMaxChunk);
    gen.movMemReg(X64Reg::RBP, kCurrentChunkBytesOff, X64Reg::RAX);
    gen.jmp(lblHaveChunk);
    gen.bindLabel(lblUseMaxChunk);
    gen.movRegImm32(X64Reg::RAX, kInstallerCopyChunkBytes);
    gen.movMemReg(X64Reg::RBP, kCurrentChunkBytesOff, X64Reg::RAX);
    gen.bindLabel(lblHaveChunk);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kHFileOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kCurrentFileOffsetOff);
    zeroLocalQword(gen, kBytesWrittenOff);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kBytesWrittenOff);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    gen.callIATSlot(setFilePointerSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentFileOffsetOff);
    gen.cmpRegReg(X64Reg::RAX, X64Reg::R10);
    gen.jnz(errorLabel);

    zeroLocalQword(gen, kBytesReadOff);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kHFileOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kPFileBufOff);
    gen.movRegMem(X64Reg::R8, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kBytesReadOff);
    storeStackImm64(gen, 0x20, 0);
    gen.callIATSlot(readSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.cmpRegReg(X64Reg::RAX, X64Reg::R10);
    gen.jnz(errorLabel);

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kCrcOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kPFileBufOff);
    gen.movRegMem(X64Reg::R8, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.callIATSlot(crcSlot);
    gen.movMemReg(X64Reg::RBP, kCrcOff, X64Reg::RAX);

    zeroLocalQword(gen, kBytesWrittenOff);
    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kHOutOff);
    gen.movRegMem(X64Reg::RDX, X64Reg::RBP, kPFileBufOff);
    gen.movRegMem(X64Reg::R8, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.leaRegMem(X64Reg::R9, X64Reg::RBP, kBytesWrittenOff);
    storeStackImm64(gen, 0x20, 0);
    gen.callIATSlot(writeSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesWrittenOff);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.cmpRegReg(X64Reg::RAX, X64Reg::R10);
    gen.jnz(errorLabel);

    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kCurrentFileOffsetOff);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.addRegReg(X64Reg::RAX, X64Reg::R10);
    gen.movMemReg(X64Reg::RBP, kCurrentFileOffsetOff, X64Reg::RAX);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kRemainingBytesOff);
    gen.movRegMem(X64Reg::R10, X64Reg::RBP, kCurrentChunkBytesOff);
    gen.subRegReg(X64Reg::RAX, X64Reg::R10);
    gen.movMemReg(X64Reg::RBP, kRemainingBytesOff, X64Reg::RAX);
    gen.jmp(lblLoop);

    gen.bindLabel(lblDone);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kCrcOff);
    emitCmpRegU32(gen, X64Reg::RAX, entry.crc32);
    gen.jnz(errorLabel);

    emitCloseLocalHandleIfSet(gen, kHOutOff, closeSlot);
    emitLocalFreeIfSet(gen, kPFileBufOff, freeSlot);
}

/// @brief Emit code to delete a single file at entry.root\entry.relativePath.
/// Treats ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, and ERROR_DIR_NOT_EMPTY as
/// non-fatal (file already absent); jumps to errorLabel on any other failure.
/// @param gen Instruction emitter.
/// @param entry File destination metadata.
/// @param slashOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param deleteSlot `DeleteFileW` slot.
/// @param getLastErrorSlot `GetLastError` slot.
/// @param errorLabel Unexpected deletion failure target.
void emitDeleteFile(InstallerStubGen &gen,
                    const WindowsPackageFileEntry &entry,
                    uint32_t slashOff,
                    uint32_t copySlot,
                    uint32_t catSlot,
                    uint32_t strlenSlot,
                    uint32_t deleteSlot,
                    uint32_t getLastErrorSlot,
                    uint32_t errorLabel) {
    const auto lblOk = gen.newLabel();
    const uint32_t relPathOff = gen.embedStringW(entry.relativePath);
    emitComposePath(gen,
                    entry.root,
                    kTempPathOff,
                    slashOff,
                    relPathOff,
                    copySlot,
                    catSlot,
                    strlenSlot,
                    errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(deleteSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblOk);
    gen.callIATSlot(getLastErrorSlot);
    gen.cmpRegImm32(X64Reg::RAX, kErrorFileNotFound);
    gen.jz(lblOk);
    gen.cmpRegImm32(X64Reg::RAX, kErrorPathNotFound);
    gen.jz(lblOk);
    gen.cmpRegImm32(X64Reg::RAX, kErrorDirNotEmpty);
    gen.jz(lblOk);
    gen.jmp(errorLabel);
    gen.bindLabel(lblOk);
}

/// @brief Emit code to remove a directory at entry.root\entry.relativePath.
/// Treats missing and non-empty directories as non-fatal so uninstall can leave
/// unrelated user content in place.
/// @param gen Instruction emitter.
/// @param entry Directory destination metadata.
/// @param slashOff Embedded separator.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param removeSlot `RemoveDirectoryW` slot.
/// @param getLastErrorSlot `GetLastError` slot.
/// @param errorLabel Unexpected removal failure target.
void emitRemoveDirectory(InstallerStubGen &gen,
                         const WindowsPackageDirEntry &entry,
                         uint32_t slashOff,
                         uint32_t copySlot,
                         uint32_t catSlot,
                         uint32_t strlenSlot,
                         uint32_t removeSlot,
                         uint32_t getLastErrorSlot,
                         uint32_t errorLabel) {
    const auto lblOk = gen.newLabel();
    const uint32_t relPathOff = gen.embedStringW(entry.relativePath);
    emitComposePath(gen,
                    entry.root,
                    kTempPathOff,
                    slashOff,
                    relPathOff,
                    copySlot,
                    catSlot,
                    strlenSlot,
                    errorLabel);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.callIATSlot(removeSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblOk);
    gen.callIATSlot(getLastErrorSlot);
    gen.cmpRegImm32(X64Reg::RAX, kErrorFileNotFound);
    gen.jz(lblOk);
    gen.cmpRegImm32(X64Reg::RAX, kErrorPathNotFound);
    gen.jz(lblOk);
    gen.cmpRegImm32(X64Reg::RAX, kErrorDirNotEmpty);
    gen.jz(lblOk);
    gen.jmp(errorLabel);
    gen.bindLabel(lblOk);
}

/// @brief Run the compressed shared PowerShell backend in a named transaction mode.
/// @details The installer path and mode are inherited through process-local environment variables,
///          avoiding Windows PowerShell's ambiguous `-EncodedCommand` argument binding. The shared
///          backend reads all overlay inputs from the signed installer and performs the journaled
///          file transaction; the native x64 wizard retains responsibility for UI and metadata.
/// @param gen Instruction emitter.
/// @param layout Package identity and backend configuration.
/// @param mode Transaction mode string.
/// @param copySlot `lstrcpyW` slot.
/// @param catSlot `lstrcatW` slot.
/// @param strlenSlot `lstrlenW` slot.
/// @param closeSlot `CloseHandle` slot.
/// @param setEnvironmentSlot `SetEnvironmentVariableW` slot.
/// @param getSystemDirectorySlot `GetSystemDirectoryW` slot.
/// @param createProcessSlot `CreateProcessW` slot.
/// @param waitForSingleObjectSlot `WaitForSingleObject` slot.
/// @param getExitCodeProcessSlot `GetExitCodeProcess` slot.
/// @param pumpUi Whether to refresh/pump the native progress dialog while waiting.
/// @param errorLabel Environment, process, timeout, or backend failure target.
void emitRunPowerShellBackend(InstallerStubGen &gen,
                              const WindowsPackageLayout &layout,
                              const std::string &mode,
                              uint32_t copySlot,
                              uint32_t catSlot,
                              uint32_t strlenSlot,
                              uint32_t closeSlot,
                              uint32_t setEnvironmentSlot,
                              uint32_t getSystemDirectorySlot,
                              uint32_t createProcessSlot,
                              uint32_t waitForSingleObjectSlot,
                              uint32_t getExitCodeProcessSlot,
                              bool pumpUi,
                              uint32_t errorLabel) {
    const std::string encodedCommand = encodedNativeTransactionPowerShellCommand(layout);
    if (encodedCommand.size() + 512u >= 32767u)
        throw std::runtime_error("Windows installer PowerShell command line is too large");

    const uint32_t selfEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_SELF");
    const uint32_t modeEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_MODE");
    const uint32_t modeEnvironmentValueOff = gen.embedStringW(mode);
    const uint32_t logEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_LOG");
    const uint32_t logEnvironmentValueOff =
        gen.embedStringW("%TEMP%\\ZannaInstaller-" + registryIdFor(layout) + ".log");
    const uint32_t progressEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_PROGRESS");
    const uint32_t progressFileNameOff =
        gen.embedStringW("ZannaInstaller-" + registryIdFor(layout) + ".progress.ini");
    const uint32_t progressSectionOff = gen.embedStringW("progress");
    const uint32_t progressPercentKeyOff = gen.embedStringW("percent");
    const uint32_t progressStatusKeyOff = gen.embedStringW("status");
    const uint32_t progressDefaultStatusOff =
        gen.embedStringW("Working on the Zanna developer toolchain...");
    const uint32_t defaultProgress = mode == "commit" ? 98u : (mode == "rollback" ? 3u : 4u);
    const uint32_t commandQuoteOff = gen.embedStringW("\"");
    const uint32_t powershellTailOff =
        gen.embedStringW("\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
                         "-ExecutionPolicy Bypass -EncodedCommand " +
                         encodedCommand);

    gen.leaRipData(X64Reg::RCX, selfEnvironmentNameOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kSelfPathOff);
    gen.callIATSlot(setEnvironmentSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    if (pumpUi) {
        gen.movRegImm32(X64Reg::RCX, kMaxPathChars);
        gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kTempPathOff);
        gen.callIATSlot(kI_GetTempPathW);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(errorLabel);
        gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
        gen.ja(errorLabel);
        emitCheckedCatEmbedded(
            gen, kTempPathOff, progressFileNameOff, catSlot, strlenSlot, errorLabel);
        gen.leaRipData(X64Reg::RCX, progressEnvironmentNameOff);
        gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kTempPathOff);
        gen.callIATSlot(setEnvironmentSlot);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(errorLabel);
    }
    gen.leaRipData(X64Reg::RCX, logEnvironmentNameOff);
    gen.leaRipData(X64Reg::RDX, logEnvironmentValueOff);
    gen.callIATSlot(setEnvironmentSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.leaRipData(X64Reg::RCX, modeEnvironmentNameOff);
    gen.leaRipData(X64Reg::RDX, modeEnvironmentValueOff);
    gen.callIATSlot(setEnvironmentSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kUninstallPathOff);
    gen.movRegImm32(X64Reg::RDX, kMaxPathChars);
    gen.callIATSlot(getSystemDirectorySlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 1);
    gen.ja(errorLabel);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kCommandBufferOff);
    gen.leaRipData(X64Reg::RDX, commandQuoteOff);
    gen.callIATSlot(copySlot);
    emitCheckedCatStack(gen, kCommandBufferOff, kUninstallPathOff, catSlot, strlenSlot, errorLabel);
    emitCheckedCatEmbedded(
        gen, kCommandBufferOff, powershellTailOff, catSlot, strlenSlot, errorLabel);

    zeroLocalRange(gen, kStartupInfoOff, 0x68);
    zeroLocalRange(gen, kProcessInfoOff, 0x18);
    zeroLocalQword(gen, kBytesReadOff);
    gen.movMemImm32(X64Reg::RBP, kStartupInfoOff, 0x68);

    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kCommandBufferOff);
    gen.xorRegReg(X64Reg::R8, X64Reg::R8);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    storeStackImm64(gen, 0x20, 0);
    storeStackImm64(gen, 0x28, kCreateNoWindow);
    storeStackImm64(gen, 0x30, 0);
    storeStackImm64(gen, 0x38, 0);
    storeStackPtrToLocal(gen, 0x40, kStartupInfoOff);
    storeStackPtrToLocal(gen, 0x48, kProcessInfoOff);
    gen.callIATSlot(createProcessSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(errorLabel);

    const auto lblProcessError = gen.newLabel();
    const auto lblProcessDone = gen.newLabel();

    if (pumpUi) {
        const auto lblWait = gen.newLabel();
        const auto lblProcessExited = gen.newLabel();
        gen.bindLabel(lblWait);
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProcessInfoOff);
        gen.movRegImm32(X64Reg::RDX, 50);
        gen.callIATSlot(waitForSingleObjectSlot);
        gen.cmpRegImm32(X64Reg::RAX, kWaitTimeout);
        gen.jz(lblProcessDone);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblProcessExited);
        gen.jmp(lblProcessError);
        gen.bindLabel(lblProcessDone);
        emitRefreshProgressUi(gen,
                              progressSectionOff,
                              progressPercentKeyOff,
                              progressStatusKeyOff,
                              progressDefaultStatusOff,
                              defaultProgress);
        emitPumpProgressMessages(gen);
        gen.jmp(lblWait);
        gen.bindLabel(lblProcessExited);
        emitRefreshProgressUi(gen,
                              progressSectionOff,
                              progressPercentKeyOff,
                              progressStatusKeyOff,
                              progressDefaultStatusOff,
                              defaultProgress);
        emitPumpProgressMessages(gen);
    } else {
        gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProcessInfoOff);
        gen.movRegImm32(X64Reg::RDX, kWaitInfinite);
        gen.callIATSlot(waitForSingleObjectSlot);
        emitCmpRegU32(gen, X64Reg::RAX, kWaitInfinite);
        gen.jz(lblProcessError);
    }

    gen.movRegMem(X64Reg::RCX, X64Reg::RBP, kProcessInfoOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kBytesReadOff);
    gen.callIATSlot(getExitCodeProcessSlot);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblProcessError);
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kBytesReadOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblProcessError);

    emitCloseLocalHandleIfSet(gen, kProcessInfoOff + 8, closeSlot);
    emitCloseLocalHandleIfSet(gen, kProcessInfoOff, closeSlot);
    const auto lblHandlesClosed = gen.newLabel();
    gen.jmp(lblHandlesClosed);

    gen.bindLabel(lblProcessError);
    emitCloseLocalHandleIfSet(gen, kProcessInfoOff + 8, closeSlot);
    emitCloseLocalHandleIfSet(gen, kProcessInfoOff, closeSlot);
    gen.jmp(errorLabel);

    gen.bindLabel(lblHandlesClosed);
}

constexpr uint32_t kArm64FrameSize = 0x33000;
constexpr uint32_t kArm64SelfPathOff = 0x1000;
constexpr uint32_t kArm64PowerShellPathOff = 0x11000;
constexpr uint32_t kArm64CommandLineOff = 0x12000;
constexpr uint32_t kArm64CommandLineBytes = 0x20000;
constexpr uint32_t kArm64StartupInfoOff = 0x32000;
constexpr uint32_t kArm64ProcessInfoOff = 0x32100;
constexpr uint32_t kArm64ExitCodeOff = 0x32120;
constexpr uint32_t kArm64StartupInfoBytes = 0x80;
constexpr uint32_t kArm64ProcessInfoBytes = 0x20;
constexpr uint32_t kStartupInfoWSize = 104;

/// @brief Emit an AArch64 address calculation for a large frame offset.
/// @param gen AArch64 instruction emitter.
/// @param dst Destination register.
/// @param off Unsigned byte offset from the saved frame base in X19.
void emitA64LeaFrame(InstallerStubGenA64 &gen, A64Reg dst, uint32_t off) {
    const uint32_t page = off & ~0xFFFu;
    const uint32_t rem = off & 0xFFFu;
    if (page != 0) {
        gen.addRegImm(dst, A64Reg::X19, page);
        if (rem != 0)
            gen.addRegImm(dst, dst, rem);
    } else {
        gen.addRegImm(dst, A64Reg::X19, rem);
    }
}

/// @brief Emit zero stores across an AArch64 frame-local range.
/// @param gen AArch64 instruction emitter.
/// @param off First byte offset from the frame base.
/// @param bytes Byte count cleared in eight-byte increments.
/// @pre @p bytes is a multiple of eight.
void emitA64ZeroFrameRange(InstallerStubGenA64 &gen, uint32_t off, uint32_t bytes) {
    emitA64LeaFrame(gen, A64Reg::X17, off);
    for (uint32_t i = 0; i < bytes; i += 8)
        gen.storeMem64(A64Reg::X17, i, A64Reg::SP);
}

/// @brief Emit storage of a 32-bit immediate in the AArch64 frame.
/// @param gen AArch64 instruction emitter.
/// @param off Destination byte offset from the frame base.
/// @param value Immediate value.
void emitA64StoreFrameImm32(InstallerStubGenA64 &gen, uint32_t off, uint32_t value) {
    emitA64LeaFrame(gen, A64Reg::X17, off);
    gen.storeMemImm32(A64Reg::X17, 0, value);
}

/// @brief Set a child-inherited environment flag when any installer flag is present.
/// @details X21 holds the original installer command line. Environment variables avoid the
///          ambiguous argument binding of Windows PowerShell's `-EncodedCommand` mode.
/// @param gen AArch64 instruction emitter.
/// @param searchOffsets Embedded command-line flag strings.
/// @param environmentNameOff Embedded environment variable name.
/// @param environmentValueOff Embedded value assigned when found.
void emitA64EnvironmentFlag(InstallerStubGenA64 &gen,
                            const std::vector<uint32_t> &searchOffsets,
                            uint32_t environmentNameOff,
                            uint32_t environmentValueOff) {
    const auto done = gen.newLabel();
    for (uint32_t searchOff : searchOffsets) {
        const auto next = gen.newLabel();
        gen.movRegReg(A64Reg::X0, A64Reg::X21);
        gen.leaData(A64Reg::X1, searchOff);
        gen.callIATSlot(kI_StrStrIW);
        gen.cbz(A64Reg::X0, next);
        gen.leaData(A64Reg::X0, environmentNameOff);
        gen.leaData(A64Reg::X1, environmentValueOff);
        gen.callIATSlot(kI_SetEnvironmentVariableW);
        gen.b(done);
        gen.bindLabel(next);
    }
    gen.bindLabel(done);
}

/// @brief Emit construction of the ARM64 System32 PowerShell command line.
/// @param gen AArch64 instruction emitter.
/// @param quoteOff Embedded quote string.
/// @param psSuffixOff Embedded PowerShell executable suffix.
/// @param afterExeOff Embedded fixed argument suffix containing EncodedCommand.
/// @param noRestartSearchOffsets Embedded `/norestart` spellings.
/// @param noRestartEnvironmentNameOff Embedded no-restart environment name.
/// @param environmentOneOff Embedded `"1"` value.
void emitA64BuildPowerShellCommand(InstallerStubGenA64 &gen,
                                   uint32_t quoteOff,
                                   uint32_t psSuffixOff,
                                   uint32_t afterExeOff,
                                   const std::vector<uint32_t> &quietSearchOffsets,
                                   uint32_t quietEnvironmentNameOff,
                                   const std::vector<uint32_t> &noRestartSearchOffsets,
                                   uint32_t noRestartEnvironmentNameOff,
                                   uint32_t environmentOneOff) {
    gen.movRegImm32(A64Reg::X0, 0);
    emitA64LeaFrame(gen, A64Reg::X1, kArm64SelfPathOff);
    gen.movRegImm32(A64Reg::X2, kMaxPathChars);
    gen.callIATSlot(kI_GetModuleFileNameW);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64PowerShellPathOff);
    gen.movRegImm32(A64Reg::X1, 2048);
    gen.callIATSlot(kI_GetSystemDirectoryW);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64PowerShellPathOff);
    gen.leaData(A64Reg::X1, psSuffixOff);
    gen.callIATSlot(kI_lstrcatW);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64CommandLineOff);
    gen.leaData(A64Reg::X1, quoteOff);
    gen.callIATSlot(kI_lstrcpyW);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64CommandLineOff);
    emitA64LeaFrame(gen, A64Reg::X1, kArm64PowerShellPathOff);
    gen.callIATSlot(kI_lstrcatW);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64CommandLineOff);
    gen.leaData(A64Reg::X1, afterExeOff);
    gen.callIATSlot(kI_lstrcatW);

    gen.callIATSlot(kI_GetCommandLineW);
    gen.movRegReg(A64Reg::X21, A64Reg::X0);
    emitA64EnvironmentFlag(gen, quietSearchOffsets, quietEnvironmentNameOff, environmentOneOff);
    emitA64EnvironmentFlag(
        gen, noRestartSearchOffsets, noRestartEnvironmentNameOff, environmentOneOff);
}

} // namespace

/// @brief Build the AArch64 installer or uninstaller bootstrap.
/// @param layout Package UI, path, and encoded-backend metadata.
/// @param uninstallDialog Whether to generate uninstall behavior.
/// @return Finalized AArch64 text, data, imports, and PE architecture.
StubResult buildArm64InstallerStub(const WindowsPackageLayout &layout, bool uninstallDialog) {
    StubResult result;
    result.imports = installerImports();
    result.peArch = "arm64";

    InstallerStubGenA64 gen;
    const std::string encodedCommand = encodedArm64PowerShellCommand(layout, uninstallDialog);
    constexpr size_t kWindowsCommandLineMaxChars = 32767u;
    const size_t minCommandChars = encodedCommand.size() + 256u;
    if (minCommandChars >= kWindowsCommandLineMaxChars ||
        minCommandChars * 2u > kArm64CommandLineBytes) {
        throw std::runtime_error("Windows installer PowerShell command line is too large (" +
                                 std::to_string(minCommandChars) + " UTF-16 characters)");
    }

    const uint32_t quoteOff = gen.embedStringW("\"");
    const uint32_t psSuffixOff = gen.embedStringW("\\WindowsPowerShell\\v1.0\\powershell.exe");
    const uint32_t afterExeOff = gen.embedStringW(
        "\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + encodedCommand);
    const uint32_t selfEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_SELF");
    const uint32_t modeEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_MODE");
    const uint32_t modeEnvironmentValueOff =
        gen.embedStringW(uninstallDialog ? "uninstall" : "install");
    const uint32_t logEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_LOG");
    const uint32_t logEnvironmentValueOff =
        gen.embedStringW("%TEMP%\\ZannaInstaller-" + registryIdFor(layout) + ".log");
    const uint32_t quietEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_QUIET");
    const uint32_t noRestartEnvironmentNameOff = gen.embedStringW("ZANNA_INSTALLER_NORESTART");
    const uint32_t environmentOneOff = gen.embedStringW("1");
    const std::vector<uint32_t> quietSearchOffsets = {gen.embedStringW("/quiet"),
                                                      gen.embedStringW("/silent"),
                                                      gen.embedStringW("-quiet"),
                                                      gen.embedStringW("-silent")};
    const std::vector<uint32_t> noRestartSearchOffsets = {gen.embedStringW("/norestart"),
                                                          gen.embedStringW("-norestart")};
    gen.embedStringW(layout.displayName + (uninstallDialog ? " Uninstall" : " Setup"));
    if (uninstallDialog) {
        // Plain UTF-16 ownership markers let a newer installer recognize and safely migrate a
        // generated pre-manifest installation without trusting filename or registry state alone.
        gen.embedStringW(registryIdFor(layout));
        gen.embedStringW(layout.installedManifestRelativePath);
    }
    const auto wizardTemplate = buildWizardDialogTemplate(layout, uninstallDialog);
    gen.embedBytes(wizardTemplate.data(), wizardTemplate.size());

    const auto lblFail = gen.newLabel();

    gen.subRegImm(A64Reg::SP, A64Reg::SP, kArm64FrameSize);
    gen.movRegReg(A64Reg::X19, A64Reg::SP);
    emitA64ZeroFrameRange(gen, kArm64StartupInfoOff, kArm64StartupInfoBytes);
    emitA64ZeroFrameRange(gen, kArm64ProcessInfoOff, kArm64ProcessInfoBytes);
    emitA64ZeroFrameRange(gen, kArm64ExitCodeOff, 8);
    emitA64StoreFrameImm32(gen, kArm64StartupInfoOff, kStartupInfoWSize);

    emitA64BuildPowerShellCommand(gen,
                                  quoteOff,
                                  psSuffixOff,
                                  afterExeOff,
                                  quietSearchOffsets,
                                  quietEnvironmentNameOff,
                                  noRestartSearchOffsets,
                                  noRestartEnvironmentNameOff,
                                  environmentOneOff);

    gen.leaData(A64Reg::X0, selfEnvironmentNameOff);
    emitA64LeaFrame(gen, A64Reg::X1, kArm64SelfPathOff);
    gen.callIATSlot(kI_SetEnvironmentVariableW);
    gen.cbz(A64Reg::X0, lblFail);
    gen.leaData(A64Reg::X0, logEnvironmentNameOff);
    gen.leaData(A64Reg::X1, logEnvironmentValueOff);
    gen.callIATSlot(kI_SetEnvironmentVariableW);
    gen.cbz(A64Reg::X0, lblFail);
    gen.leaData(A64Reg::X0, modeEnvironmentNameOff);
    gen.leaData(A64Reg::X1, modeEnvironmentValueOff);
    gen.callIATSlot(kI_SetEnvironmentVariableW);
    gen.cbz(A64Reg::X0, lblFail);

    emitA64LeaFrame(gen, A64Reg::X17, kArm64StartupInfoOff);
    gen.storeMem64(A64Reg::X19, 0, A64Reg::X17);
    emitA64LeaFrame(gen, A64Reg::X17, kArm64ProcessInfoOff);
    gen.storeMem64(A64Reg::X19, 8, A64Reg::X17);

    emitA64LeaFrame(gen, A64Reg::X0, kArm64PowerShellPathOff);
    emitA64LeaFrame(gen, A64Reg::X1, kArm64CommandLineOff);
    gen.movRegImm32(A64Reg::X2, 0);
    gen.movRegImm32(A64Reg::X3, 0);
    gen.movRegImm32(A64Reg::X4, 0);
    gen.movRegImm32(A64Reg::X5, kCreateNoWindow);
    gen.movRegImm32(A64Reg::X6, 0);
    gen.movRegImm32(A64Reg::X7, 0);
    gen.callIATSlot(kI_CreateProcessW);
    gen.cbz(A64Reg::X0, lblFail);

    emitA64LeaFrame(gen, A64Reg::X20, kArm64ProcessInfoOff);
    gen.loadMem64(A64Reg::X0, A64Reg::X20, 0);
    gen.movRegImm32(A64Reg::X1, kWaitInfinite);
    gen.callIATSlot(kI_WaitForSingleObject);

    gen.loadMem64(A64Reg::X0, A64Reg::X20, 0);
    emitA64LeaFrame(gen, A64Reg::X1, kArm64ExitCodeOff);
    gen.callIATSlot(kI_GetExitCodeProcess);
    gen.cbz(A64Reg::X0, lblFail);

    gen.loadMem64(A64Reg::X0, A64Reg::X20, 8);
    gen.callIATSlot(kI_CloseHandle);
    gen.loadMem64(A64Reg::X0, A64Reg::X20, 0);
    gen.callIATSlot(kI_CloseHandle);

    emitA64LeaFrame(gen, A64Reg::X17, kArm64ExitCodeOff);
    gen.loadMem64(A64Reg::X0, A64Reg::X17, 0);
    gen.callIATSlot(kI_ExitProcess);

    gen.bindLabel(lblFail);
    gen.movRegImm32(A64Reg::X0, 1);
    gen.callIATSlot(kI_ExitProcess);

    finalizeStubRVAs(result, gen);
    result.stubData = gen.dataSection();
    return result;
}

/// @brief Build the complete x86-64 installer stub for the given package layout.
/// Assembles all install steps: root path resolution, optional clean of existing install,
/// directory creation, file extraction with CRC verification, registry entries (uninstall key,
/// file associations, shortcuts), and PATH update. Jumps to an error handler on any failure
/// that displays a MessageBox and exits with code 1.
/// @param layout Complete installer metadata and overlay map.
/// @param arch Requested payload/bootstrap architecture.
/// @return Finalized architecture-specific installer stub.
/// @throws std::runtime_error For unsupported architecture or invalid generation metadata.
StubResult buildInstallerStub(const WindowsPackageLayout &layout, const std::string &arch) {
    if (resolveBootstrapArch(arch) == "arm64")
        return buildArm64InstallerStub(layout, false);

    StubResult result;
    result.imports = installerImports();
    result.peArch = resolveBootstrapArch(arch);

    InstallerStubGen gen;

    const std::string installDir = installDirNameFor(layout);
    const std::string version = layout.version.empty() ? "0.0.0" : layout.version;
    const std::string publisher = layout.publisher.empty() ? "Zanna" : layout.publisher;
    const std::string uninstallKey = uninstallKeyPathFor(layout);
    const uint64_t registryRoot = registryRootFor(layout);

    const uint32_t slashOff = gen.embedStringW("\\");
    const uint32_t starOff = gen.embedStringW("*");
    const uint32_t semicolonOff = gen.embedStringW(";");
    const uint32_t quoteOff = gen.embedStringW("\"");
    const uint32_t installDirOff = gen.embedStringW(installDir);
    const uint32_t displayNameOff = gen.embedStringW(layout.displayName);
    const uint32_t versionOff = gen.embedStringW(version);
    const uint32_t publisherOff = gen.embedStringW(publisher);
    const uint32_t uninstallKeyOff = gen.embedStringW(uninstallKey);
    const uint32_t uninstallExeOff = gen.embedStringW("uninstall.exe");
    const uint32_t environmentKeyOff = gen.embedStringW(environmentKeyPathFor(layout));
    const uint32_t environmentOff = gen.embedStringW("Environment");
    const uint32_t quietSlashOff = gen.embedStringW("/quiet");
    const uint32_t silentSlashOff = gen.embedStringW("/silent");
    const uint32_t quietDashOff = gen.embedStringW("-quiet");
    const uint32_t silentDashOff = gen.embedStringW("-silent");
    const uint32_t noRestartSlashOff = gen.embedStringW("/norestart");
    const uint32_t noRestartDashOff = gen.embedStringW("-norestart");
    const uint32_t quietUninstallArgsOff = gen.embedStringW(" /quiet");

    const uint32_t regDisplayNameOff = gen.embedStringW("DisplayName");
    const uint32_t regDisplayVersionOff = gen.embedStringW("DisplayVersion");
    const uint32_t regPublisherOff = gen.embedStringW("Publisher");
    const uint32_t regInstallLocationOff = gen.embedStringW("InstallLocation");
    const uint32_t regUninstallStringOff = gen.embedStringW("UninstallString");
    const uint32_t regQuietUninstallStringOff = gen.embedStringW("QuietUninstallString");
    const uint32_t regDisplayIconOff = gen.embedStringW("DisplayIcon");
    const uint32_t regNoModifyOff = gen.embedStringW("NoModify");
    const uint32_t regNoRepairOff = gen.embedStringW("NoRepair");
    const uint32_t regEstimatedSizeOff = gen.embedStringW("EstimatedSize");
    const uint32_t regInstallDateOff = gen.embedStringW("InstallDate");
    const uint32_t regUrlInfoAboutOff = gen.embedStringW("URLInfoAbout");
    const uint32_t regUrlUpdateInfoOff = gen.embedStringW("URLUpdateInfo");
    const uint32_t regHelpLinkOff = gen.embedStringW("HelpLink");
    const uint32_t regCommentsOff = gen.embedStringW("Comments");
    const uint32_t regContactOff = gen.embedStringW("Contact");
    const uint32_t regShortcutPathsOff = gen.embedStringW("ZAPSShortcutPaths");
    const uint32_t regPathValueOff = gen.embedStringW("Path");
    const uint32_t regOriginalPathOff = gen.embedStringW("ZAPSOriginalPath");
    const uint32_t regPathEntryOff = gen.embedStringW("ZAPSPathEntry");
    const uint32_t installDateFormatOff = gen.embedStringW("yyyyMMdd");
    const uint32_t nativeStageNameOff = gen.embedStringW("ZANNA_INSTALLER_NATIVE_STAGE");
    const uint32_t nativePathStageOff = gen.embedStringW("PATH registration");
    const uint32_t nativeUninstallStageOff = gen.embedStringW("Add/Remove Programs registration");
    const uint32_t nativeAssociationsStageOff = gen.embedStringW("file association registration");
    const uint32_t nativeCommitStageOff = gen.embedStringW("transaction commit");
    const uint32_t progressPreparingOff =
        gen.embedStringW("Preparing the selected Zanna developer components...");
    const uint32_t progressPathsOff =
        gen.embedStringW("Preparing installation paths and upgrade ownership...");
    const uint32_t progressPathRegistrationOff =
        gen.embedStringW("Registering Zanna tools on the developer PATH...");
    const uint32_t progressMetadataOff =
        gen.embedStringW("Registering Zanna in Windows Apps & Features...");
    const uint32_t progressAssociationsOff =
        gen.embedStringW("Registering Zanna source and project file types...");
    const uint32_t progressFinalizingOff = gen.embedStringW("Finalizing installation...");
    const uint32_t progressRollbackOff =
        gen.embedStringW("Rolling back incomplete installation changes...");
    const uint32_t progressCompleteOff = gen.embedStringW("Installation complete.");
    /// @brief Publish the current native installation stage in the process environment.
    /// @param stageOff Embedded-string offset naming the stage.
    const auto emitNativeStage = [&](uint32_t stageOff) {
        gen.leaRipData(X64Reg::RCX, nativeStageNameOff);
        gen.leaRipData(X64Reg::RDX, stageOff);
        gen.callIATSlot(kI_SetEnvironmentVariableW);
    };

    const std::string successTitle = layout.displayName + " Setup";
    const std::string successMessage =
        "Installation complete. " + layout.displayName +
        " has been installed.\r\n\r\nOpen a new terminal to use PATH updates, or launch "
        "Zanna Studio from the Start Menu if shortcuts were enabled.";
    const std::string errorTitle = "Setup Error";
    const std::string errorMessage =
        "Installation could not be completed. Any in-progress changes were rolled back.\r\n\r\n"
        "Details were written to:\r\n%TEMP%\\ZannaInstaller-" +
        registryIdFor(layout) + ".log";
    const uint32_t darkThemeOff = gen.embedStringW("DarkMode_Explorer");
    const WizardDibData wizardDib = buildWizardDib(layout);
    const uint32_t wizardDibInfoOff =
        wizardDib.infoHeader.empty()
            ? 0
            : gen.embedBytes(wizardDib.infoHeader.data(), wizardDib.infoHeader.size());
    const uint32_t wizardDibPixelsOff =
        wizardDib.bgraPixels.empty()
            ? 0
            : gen.embedBytes(wizardDib.bgraPixels.data(), wizardDib.bgraPixels.size());
    const auto wizardTemplate = buildWizardDialogTemplate(layout, false);
    const uint32_t wizardTemplateOff = gen.embedBytes(wizardTemplate.data(), wizardTemplate.size());
    const auto progressTemplate = buildProgressDialogTemplate(layout);
    const uint32_t progressTemplateOff =
        gen.embedBytes(progressTemplate.data(), progressTemplate.size());
    const bool branded = !wizardDib.bgraPixels.empty();
    const auto successTemplate = buildResultDialogTemplate(successTitle, successMessage, branded);
    const uint32_t successTemplateOff =
        gen.embedBytes(successTemplate.data(), successTemplate.size());
    const auto errorTemplate = buildResultDialogTemplate(errorTitle, errorMessage, branded);
    const uint32_t errorTemplateOff = gen.embedBytes(errorTemplate.data(), errorTemplate.size());

    const auto lblError = gen.newLabel();
    const auto lblRollbackError = gen.newLabel();
    const auto lblCleanupSuccess = gen.newLabel();
    const auto lblCleanupCancelled = gen.newLabel();
    const auto lblCleanupError = gen.newLabel();
    const auto lblCleanupRollback = gen.newLabel();
    const auto lblExitSuccess = gen.newLabel();
    const auto lblExitCancelled = gen.newLabel();
    const auto lblExitError = gen.newLabel();
    const auto lblWizardDialogProc = gen.newLabel();

    gen.push(X64Reg::RBP);
    gen.movRegReg(X64Reg::RBP, X64Reg::RSP);
    gen.subRegImm32(X64Reg::RSP, kFrameSize);

    zeroLocalQword(gen, kHFileOff);
    zeroLocalQword(gen, kHOutOff);
    zeroLocalQword(gen, kPFileBufOff);
    zeroLocalQword(gen, kRegKeyOff);
    zeroLocalQword(gen, kPathUpdatedOff);
    zeroLocalQword(gen, kQuietModeOff);
    zeroLocalQword(gen, kNoRestartModeOff);
    zeroLocalQword(gen, kKnownFolderPtrOff);
    zeroLocalQword(gen, kProgressHwndOff);
    zeroLocalQword(gen, kProgressPathReadyOff);

    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kSelfPathOff);
    gen.movRegImm32(X64Reg::R8, kMaxPathChars);
    gen.callIATSlot(kI_GetModuleFileNameW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblError);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 2);
    gen.ja(lblError);

    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kSelfPathOff);
    gen.movRegImm32(X64Reg::RDX, kGenericRead);
    gen.movRegImm32(X64Reg::R8, kFileShareRead);
    gen.xorRegReg(X64Reg::R9, X64Reg::R9);
    storeStackImm64(gen, 0x20, kOpenExisting);
    storeStackImm64(gen, 0x28, 0);
    storeStackImm64(gen, 0x30, 0);
    gen.callIATSlot(kI_CreateFileW);
    gen.cmpRegImm32(X64Reg::RAX, 0xFFFFFFFFu);
    gen.jz(lblError);
    gen.movMemReg(X64Reg::RBP, kHFileOff, X64Reg::RAX);

    emitDetectQuietMode(
        gen,
        kI_GetCommandLineW,
        kI_StrStrIW,
        {{quietSlashOff, 12}, {silentSlashOff, 14}, {quietDashOff, 12}, {silentDashOff, 14}});
    emitDetectFlagMode(gen,
                       kI_GetCommandLineW,
                       kI_StrStrIW,
                       {{noRestartSlashOff, 20}, {noRestartDashOff, 20}},
                       kNoRestartModeOff);
    emitInitializeComponentSelections(gen, layout, kI_GetCommandLineW, kI_StrStrIW);

    emitWizardDialog(gen,
                     wizardTemplateOff,
                     lblWizardDialogProc,
                     kI_InitCommonControlsEx,
                     kI_DialogBoxIndirectParamW,
                     lblCleanupCancelled,
                     lblError);

    emitComponentSelectionEnvironment(gen, layout, kI_SetEnvironmentVariableW, lblError);
    emitCreateProgressDialog(gen, layout, progressTemplateOff, lblWizardDialogProc, lblError);
    emitSetProgressUi(gen, 1, progressPreparingOff);

    emitBuildRootPaths(gen,
                       layout,
                       slashOff,
                       installDirOff,
                       kI_lstrcpyW,
                       kI_lstrcatW,
                       kI_lstrlenW,
                       kI_CreateDirectoryW,
                       kI_GetLastError,
                       kI_SHGetKnownFolderPath,
                       kI_CoTaskMemFree,
                       true,
                       lblError);

    emitSetProgressUi(gen, 3, progressPathsOff);

    emitCleanInstallRootContents(gen,
                                 layout,
                                 slashOff,
                                 starOff,
                                 kI_lstrcpyW,
                                 kI_lstrcatW,
                                 kI_lstrlenW,
                                 kI_SHFileOperationW,
                                 lblRollbackError);

    if (!layout.compressedPayloadRelativePath.empty()) {
        if (layout.compressedPayloadManifestRelativePath.empty() ||
            layout.installedManifestRelativePath.empty()) {
            throw std::runtime_error("compressed Windows payload requires install manifests");
        }
        emitRunPowerShellBackend(gen,
                                 layout,
                                 "install-files",
                                 kI_lstrcpyW,
                                 kI_lstrcatW,
                                 kI_lstrlenW,
                                 kI_CloseHandle,
                                 kI_SetEnvironmentVariableW,
                                 kI_GetSystemDirectoryW,
                                 kI_CreateProcessW,
                                 kI_WaitForSingleObject,
                                 kI_GetExitCodeProcess,
                                 true,
                                 lblError);
    } else {
        for (const auto &dir : layout.installDirectories) {
            emitCreateDirectoryAtRoot(gen,
                                      dir.root,
                                      slashOff,
                                      gen.embedStringW(dir.relativePath),
                                      kI_lstrcpyW,
                                      kI_lstrcatW,
                                      kI_lstrlenW,
                                      kI_CreateDirectoryW,
                                      kI_GetLastError,
                                      lblRollbackError);
        }

        for (const auto &file : layout.installFiles) {
            emitExtractFile(gen,
                            file,
                            layout.overlayFileOffset,
                            slashOff,
                            kI_lstrcpyW,
                            kI_lstrcatW,
                            kI_lstrlenW,
                            kI_CreateFileW,
                            kI_ReadFile,
                            kI_WriteFile,
                            kI_CloseHandle,
                            kI_LocalAlloc,
                            kI_LocalFree,
                            kI_SetFilePointerEx,
                            kI_RtlComputeCrc32,
                            lblRollbackError);
        }
    }

    emitSetProgressUi(gen, 93, progressPathRegistrationOff);
    emitNativeStage(nativePathStageOff);
    emitInstallPathUpdate(gen,
                          layout,
                          slashOff,
                          semicolonOff,
                          uninstallKeyOff,
                          environmentKeyOff,
                          regPathValueOff,
                          regOriginalPathOff,
                          regPathEntryOff,
                          environmentOff,
                          kI_RegCreateKeyW,
                          kI_RegOpenKeyW,
                          kI_RegQueryValueExW,
                          kI_RegSetValueExW,
                          kI_RegCloseKey,
                          kI_lstrcpyW,
                          kI_lstrcatW,
                          kI_lstrcmpiW,
                          kI_lstrlenW,
                          kI_SendMessageTimeoutW,
                          lblRollbackError);

    emitSetProgressUi(gen, 95, progressMetadataOff);
    emitNativeStage(nativeUninstallStageOff);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kUninstallPathOff);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kInstallPathOff);
    gen.callIATSlot(kI_lstrcpyW);
    emitCheckedCatEmbedded(
        gen, kUninstallPathOff, slashOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
    emitCheckedCatEmbedded(
        gen, kUninstallPathOff, uninstallExeOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);

    gen.movRegImm64(X64Reg::RCX, registryRoot);
    gen.leaRipData(X64Reg::RDX, uninstallKeyOff);
    gen.leaRegMem(X64Reg::R8, X64Reg::RBP, kRegKeyOff);
    gen.callIATSlot(kI_RegCreateKeyW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblRollbackError);

    emitRegSetConstString(gen,
                          kI_RegSetValueExW,
                          regDisplayNameOff,
                          displayNameOff,
                          wideBytesFor(layout.displayName),
                          lblRollbackError);
    emitRegSetConstString(gen,
                          kI_RegSetValueExW,
                          regDisplayVersionOff,
                          versionOff,
                          wideBytesFor(version),
                          lblRollbackError);
    emitRegSetConstString(gen,
                          kI_RegSetValueExW,
                          regPublisherOff,
                          publisherOff,
                          wideBytesFor(publisher),
                          lblRollbackError);
    emitRegSetStackString(gen,
                          kI_RegSetValueExW,
                          kI_lstrlenW,
                          regInstallLocationOff,
                          kInstallPathOff,
                          lblRollbackError);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
    gen.leaRipData(X64Reg::RDX, quoteOff);
    gen.callIATSlot(kI_lstrcpyW);
    emitCheckedCatStack(
        gen, kTempPathOff, kUninstallPathOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
    emitCheckedCatEmbedded(gen, kTempPathOff, quoteOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
    emitRegSetStackString(
        gen, kI_RegSetValueExW, kI_lstrlenW, regUninstallStringOff, kTempPathOff, lblRollbackError);
    emitCheckedCatEmbedded(
        gen, kTempPathOff, quietUninstallArgsOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
    emitRegSetStackString(gen,
                          kI_RegSetValueExW,
                          kI_lstrlenW,
                          regQuietUninstallStringOff,
                          kTempPathOff,
                          lblRollbackError);

    if (!layout.displayIconRelativePath.empty()) {
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kTempPathOff);
        gen.leaRipData(X64Reg::RDX, quoteOff);
        gen.callIATSlot(kI_lstrcpyW);
        emitCheckedCatStack(
            gen, kTempPathOff, kInstallPathOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
        emitCheckedCatEmbedded(
            gen, kTempPathOff, slashOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
        emitCheckedCatEmbedded(gen,
                               kTempPathOff,
                               gen.embedStringW(layout.displayIconRelativePath),
                               kI_lstrcatW,
                               kI_lstrlenW,
                               lblRollbackError);
        emitCheckedCatEmbedded(
            gen, kTempPathOff, quoteOff, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
        emitRegSetStackString(
            gen, kI_RegSetValueExW, kI_lstrlenW, regDisplayIconOff, kTempPathOff, lblRollbackError);
    }
    emitRegSetConstDword(gen, kI_RegSetValueExW, regNoModifyOff, 1, lblRollbackError);
    emitRegSetConstDword(gen, kI_RegSetValueExW, regNoRepairOff, 1, lblRollbackError);
    if (layout.estimatedSizeKb != 0)
        emitRegSetConstDword(
            gen, kI_RegSetValueExW, regEstimatedSizeOff, layout.estimatedSizeKb, lblRollbackError);
    if (!layout.installDate.empty()) {
        const uint32_t installDateOff = gen.embedStringW(layout.installDate);
        emitRegSetCurrentInstallDate(gen,
                                     kI_GetDateFormatW,
                                     kI_lstrcpyW,
                                     kI_RegSetValueExW,
                                     kI_lstrlenW,
                                     regInstallDateOff,
                                     installDateFormatOff,
                                     installDateOff,
                                     lblRollbackError);
    }
    if (!layout.homepage.empty()) {
        const uint32_t homepageOff = gen.embedStringW(layout.homepage);
        emitRegSetConstString(gen,
                              kI_RegSetValueExW,
                              regUrlInfoAboutOff,
                              homepageOff,
                              wideBytesFor(layout.homepage),
                              lblRollbackError);
        emitRegSetConstString(gen,
                              kI_RegSetValueExW,
                              regUrlUpdateInfoOff,
                              homepageOff,
                              wideBytesFor(layout.homepage),
                              lblRollbackError);
        emitRegSetConstString(gen,
                              kI_RegSetValueExW,
                              regHelpLinkOff,
                              homepageOff,
                              wideBytesFor(layout.homepage),
                              lblRollbackError);
    }
    if (!layout.description.empty()) {
        const uint32_t commentsOff = gen.embedStringW(layout.description);
        emitRegSetConstString(gen,
                              kI_RegSetValueExW,
                              regCommentsOff,
                              commentsOff,
                              wideBytesFor(layout.description),
                              lblRollbackError);
    }
    if (!layout.contact.empty()) {
        const uint32_t contactOff = gen.embedStringW(layout.contact);
        emitRegSetConstString(gen,
                              kI_RegSetValueExW,
                              regContactOff,
                              contactOff,
                              wideBytesFor(layout.contact),
                              lblRollbackError);
    }
    emitComposeSelectedShortcutOwnership(
        gen, layout, kTempPathOff, kI_lstrcpyW, kI_lstrcatW, kI_lstrlenW, lblRollbackError);
    emitRegSetStackString(
        gen, kI_RegSetValueExW, kI_lstrlenW, regShortcutPathsOff, kTempPathOff, lblRollbackError);
    if (layout.addToPath) {
        const auto lblSkipPathMetadata = gen.newLabel();
        gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kPathUpdatedOff);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jz(lblSkipPathMetadata);
        emitRegSetStackString(gen,
                              kI_RegSetValueExW,
                              kI_lstrlenW,
                              regOriginalPathOff,
                              kPathOriginalOff,
                              kRegExpandSz,
                              lblRollbackError);
        emitComposeInstallPathEntry(gen,
                                    layout,
                                    kTempPathOff,
                                    slashOff,
                                    kI_lstrcpyW,
                                    kI_lstrcatW,
                                    kI_lstrlenW,
                                    lblRollbackError);
        emitRegSetStackString(gen,
                              kI_RegSetValueExW,
                              kI_lstrlenW,
                              regPathEntryOff,
                              kTempPathOff,
                              kRegExpandSz,
                              lblRollbackError);
        gen.bindLabel(lblSkipPathMetadata);
    }
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);

    emitSetProgressUi(gen, 97, progressAssociationsOff);
    emitNativeStage(nativeAssociationsStageOff);
    emitRegisterFileAssociations(gen,
                                 layout,
                                 slashOff,
                                 kI_lstrcpyW,
                                 kI_lstrcatW,
                                 kI_RegCreateKeyW,
                                 kI_RegSetValueExW,
                                 kI_RegQueryValueExW,
                                 kI_lstrcmpW,
                                 kI_RegCloseKey,
                                 kI_lstrlenW,
                                 lblRollbackError);

    if (!layout.compressedPayloadRelativePath.empty()) {
        emitSetProgressUi(gen, 98, progressFinalizingOff);
        emitNativeStage(nativeCommitStageOff);
        emitRunPowerShellBackend(gen,
                                 layout,
                                 "commit",
                                 kI_lstrcpyW,
                                 kI_lstrcatW,
                                 kI_lstrlenW,
                                 kI_CloseHandle,
                                 kI_SetEnvironmentVariableW,
                                 kI_GetSystemDirectoryW,
                                 kI_CreateProcessW,
                                 kI_WaitForSingleObject,
                                 kI_GetExitCodeProcess,
                                 true,
                                 lblRollbackError);
    }

    emitSetProgressUi(gen, 100, progressCompleteOff);
    emitPumpProgressMessages(gen);
    emitDestroyProgressDialog(gen, layout);
    emitResultDialogUnlessQuiet(
        gen, successTemplateOff, lblWizardDialogProc, kI_DialogBoxIndirectParamW);
    gen.jmp(lblCleanupSuccess);

    gen.bindLabel(lblError);
    emitDestroyProgressDialog(gen, layout);
    emitResultDialogUnlessQuiet(
        gen, errorTemplateOff, lblWizardDialogProc, kI_DialogBoxIndirectParamW);
    gen.jmp(lblCleanupError);

    gen.bindLabel(lblRollbackError);
    emitSetProgressUi(gen, 3, progressRollbackOff);
    gen.jmp(lblCleanupRollback);

    gen.bindLabel(lblCleanupSuccess);
    emitCloseLocalHandleIfSet(gen, kHOutOff, kI_CloseHandle);
    emitLocalFreeIfSet(gen, kPFileBufOff, kI_LocalFree);
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);
    emitCloseLocalHandleIfSet(gen, kHFileOff, kI_CloseHandle);
    gen.jmp(lblExitSuccess);

    gen.bindLabel(lblCleanupCancelled);
    emitCloseLocalHandleIfSet(gen, kHOutOff, kI_CloseHandle);
    emitLocalFreeIfSet(gen, kPFileBufOff, kI_LocalFree);
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);
    emitCloseLocalHandleIfSet(gen, kHFileOff, kI_CloseHandle);
    gen.jmp(lblExitCancelled);

    gen.bindLabel(lblCleanupError);
    emitCloseLocalHandleIfSet(gen, kHOutOff, kI_CloseHandle);
    emitLocalFreeIfSet(gen, kPFileBufOff, kI_LocalFree);
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);
    emitCloseLocalHandleIfSet(gen, kHFileOff, kI_CloseHandle);
    gen.jmp(lblExitError);

    gen.bindLabel(lblCleanupRollback);
    emitCloseLocalHandleIfSet(gen, kHOutOff, kI_CloseHandle);
    emitLocalFreeIfSet(gen, kPFileBufOff, kI_LocalFree);
    emitRegCloseIfSet(gen, kRegKeyOff, kI_RegCloseKey);
    emitCloseLocalHandleIfSet(gen, kHFileOff, kI_CloseHandle);
    emitRestorePathFromOriginalLocal(gen,
                                     layout,
                                     environmentKeyOff,
                                     regPathValueOff,
                                     environmentOff,
                                     kI_RegCreateKeyW,
                                     kI_RegSetValueExW,
                                     kI_RegCloseKey,
                                     kI_lstrlenW,
                                     kI_SendMessageTimeoutW);
    if (layout.compressedPayloadRelativePath.empty()) {
        for (const auto &file : layout.uninstallFiles) {
            const auto lblNextRollbackDelete = gen.newLabel();
            emitDeleteFile(gen,
                           file,
                           slashOff,
                           kI_lstrcpyW,
                           kI_lstrcatW,
                           kI_lstrlenW,
                           kI_DeleteFileW,
                           kI_GetLastError,
                           lblNextRollbackDelete);
            gen.bindLabel(lblNextRollbackDelete);
        }
        {
            const auto lblNextRollbackDelete = gen.newLabel();
            emitDeleteFile(
                gen,
                WindowsPackageFileEntry{WindowsInstallRoot::InstallDir, "uninstall.exe", 0, 0},
                slashOff,
                kI_lstrcpyW,
                kI_lstrcatW,
                kI_lstrlenW,
                kI_DeleteFileW,
                kI_GetLastError,
                lblNextRollbackDelete);
            gen.bindLabel(lblNextRollbackDelete);
        }
        for (const auto &dir : layout.uninstallDirectories) {
            const auto lblNextRollbackRemoveDir = gen.newLabel();
            emitRemoveDirectory(gen,
                                dir,
                                slashOff,
                                kI_lstrcpyW,
                                kI_lstrcatW,
                                kI_lstrlenW,
                                kI_RemoveDirectoryW,
                                kI_GetLastError,
                                lblNextRollbackRemoveDir);
            gen.bindLabel(lblNextRollbackRemoveDir);
        }
        if (needsMenuPath(layout)) {
            gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMenuPathOff);
            gen.callIATSlot(kI_RemoveDirectoryW);
        }
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kInstallPathOff);
        gen.callIATSlot(kI_RemoveDirectoryW);
    }
    emitUnregisterFileAssociations(gen,
                                   layout,
                                   kI_RegOpenKeyW,
                                   kI_RegCloseKey,
                                   kI_RegQueryValueExW,
                                   kI_lstrcmpW,
                                   kI_RegDeleteValueW,
                                   kI_RegDeleteTreeW);
    emitRegDeleteConstKey(gen, kI_RegDeleteTreeW, uninstallKeyOff, registryRoot);
    if (!layout.compressedPayloadRelativePath.empty()) {
        const auto lblRollbackBackendDone = gen.newLabel();
        emitRunPowerShellBackend(gen,
                                 layout,
                                 "rollback",
                                 kI_lstrcpyW,
                                 kI_lstrcatW,
                                 kI_lstrlenW,
                                 kI_CloseHandle,
                                 kI_SetEnvironmentVariableW,
                                 kI_GetSystemDirectoryW,
                                 kI_CreateProcessW,
                                 kI_WaitForSingleObject,
                                 kI_GetExitCodeProcess,
                                 true,
                                 lblRollbackBackendDone);
        gen.bindLabel(lblRollbackBackendDone);
    }
    emitDestroyProgressDialog(gen, layout);
    emitResultDialogUnlessQuiet(
        gen, errorTemplateOff, lblWizardDialogProc, kI_DialogBoxIndirectParamW);
    gen.jmp(lblExitError);

    gen.bindLabel(lblExitSuccess);
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.callIATSlot(kI_ExitProcess);

    gen.bindLabel(lblExitCancelled);
    gen.movRegImm32(X64Reg::RCX, kErrorInstallUserExit);
    gen.callIATSlot(kI_ExitProcess);

    gen.bindLabel(lblExitError);
    gen.movRegImm32(X64Reg::RCX, 1);
    gen.callIATSlot(kI_ExitProcess);

    emitWizardDialogProc(gen,
                         layout,
                         lblWizardDialogProc,
                         kI_EndDialog,
                         kI_GetDlgItem,
                         kI_SendMessageW,
                         kI_EnableWindow,
                         darkThemeOff,
                         kI_GetDlgCtrlID,
                         kI_SetWindowTheme,
                         kI_DwmSetWindowAttribute,
                         kI_GetStockObject,
                         kI_SetDCBrushColor,
                         kI_SetTextColor,
                         kI_SetBkColor,
                         kI_SetWindowLongPtrW,
                         kI_GetWindowLongPtrW,
                         kI_StretchDIBits,
                         wizardDibInfoOff,
                         wizardDibPixelsOff);

    finalizeStubRVAs(result, gen);
    result.stubData = gen.dataSection();
    return result;
}

/// @brief Build the complete x86-64 uninstaller stub for the given package layout.
/// Assembles all uninstall steps: root path resolution, file deletion, directory removal,
/// registry cleanup (uninstall key, file associations, shortcuts), and PATH restoration.
/// On failure shows a MessageBox and exits with code 1.
/// @param layout Complete uninstall ownership and metadata map.
/// @param arch Requested payload/bootstrap architecture.
/// @return Finalized architecture-specific uninstaller stub.
/// @throws std::runtime_error For unsupported architecture or invalid generation metadata.
StubResult buildUninstallerStub(const WindowsPackageLayout &layout, const std::string &arch) {
    if (resolveBootstrapArch(arch) == "arm64")
        return buildArm64InstallerStub(layout, true);

    StubResult result;
    result.imports = uninstallerImports();
    result.peArch = resolveBootstrapArch(arch);

    InstallerStubGen gen;

    const std::string installDir = installDirNameFor(layout);
    const std::string uninstallKey = uninstallKeyPathFor(layout);
    const uint64_t registryRoot = registryRootFor(layout);

    const uint32_t slashOff = gen.embedStringW("\\");
    const uint32_t semicolonOff = gen.embedStringW(";");
    const uint32_t installDirOff = gen.embedStringW(installDir);
    const uint32_t uninstallKeyOff = gen.embedStringW(uninstallKey);
    const uint32_t environmentKeyOff = gen.embedStringW(environmentKeyPathFor(layout));
    const uint32_t environmentOff = gen.embedStringW("Environment");
    const uint32_t regPathValueOff = gen.embedStringW("Path");
    const uint32_t regPathEntryOff = gen.embedStringW("ZAPSPathEntry");
    const uint32_t quietSlashOff = gen.embedStringW("/quiet");
    const uint32_t silentSlashOff = gen.embedStringW("/silent");
    const uint32_t quietDashOff = gen.embedStringW("-quiet");
    const uint32_t silentDashOff = gen.embedStringW("-silent");
    const uint32_t noRestartSlashOff = gen.embedStringW("/norestart");
    const uint32_t noRestartDashOff = gen.embedStringW("-norestart");
    const uint32_t successTitleOff = gen.embedStringW(layout.displayName + " Uninstall");
    const uint32_t successMsgOff = gen.embedStringW(layout.displayName + " has been uninstalled.");
    const uint32_t errorTitleOff = gen.embedStringW("Uninstall Error");
    const uint32_t errorMsgOff =
        gen.embedStringW("Uninstall failed. Required installation paths could not be resolved.");
    const uint32_t darkThemeOff = gen.embedStringW("DarkMode_Explorer");
    // Keep the generated-owner proof readable in the PE even though the transaction script is
    // compressed. LegacyInstallOwned requires both markers before it will replace old files.
    gen.embedStringW(registryIdFor(layout));
    gen.embedStringW(layout.installedManifestRelativePath);
    const auto wizardTemplate = buildWizardDialogTemplate(layout, true);
    const uint32_t wizardTemplateOff = gen.embedBytes(wizardTemplate.data(), wizardTemplate.size());
    const auto lblError = gen.newLabel();
    const auto lblExitSuccess = gen.newLabel();
    const auto lblExitCancelled = gen.newLabel();
    const auto lblExitError = gen.newLabel();
    const auto lblWizardDialogProc = gen.newLabel();

    gen.push(X64Reg::RBP);
    gen.movRegReg(X64Reg::RBP, X64Reg::RSP);
    gen.subRegImm32(X64Reg::RSP, kFrameSize);
    zeroLocalQword(gen, kRegKeyOff);
    zeroLocalQword(gen, kQuietModeOff);
    zeroLocalQword(gen, kNoRestartModeOff);
    zeroLocalQword(gen, kKnownFolderPtrOff);

    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.leaRegMem(X64Reg::RDX, X64Reg::RBP, kSelfPathOff);
    gen.movRegImm32(X64Reg::R8, kMaxPathChars);
    gen.callIATSlot(kU_GetModuleFileNameW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jz(lblError);
    gen.cmpRegImm32(X64Reg::RAX, kMaxPathChars - 2);
    gen.ja(lblError);

    emitDetectQuietMode(
        gen,
        kU_GetCommandLineW,
        kU_StrStrIW,
        {{quietSlashOff, 12}, {silentSlashOff, 14}, {quietDashOff, 12}, {silentDashOff, 14}});
    emitDetectFlagMode(gen,
                       kU_GetCommandLineW,
                       kU_StrStrIW,
                       {{noRestartSlashOff, 20}, {noRestartDashOff, 20}},
                       kNoRestartModeOff);

    emitWizardDialog(gen,
                     wizardTemplateOff,
                     lblWizardDialogProc,
                     kU_InitCommonControlsEx,
                     kU_DialogBoxIndirectParamW,
                     lblExitCancelled,
                     lblError);

    emitBuildRootPaths(gen,
                       layout,
                       slashOff,
                       installDirOff,
                       kU_lstrcpyW,
                       kU_lstrcatW,
                       kU_lstrlenW,
                       kU_RemoveDirectoryW,
                       0,
                       kU_SHGetKnownFolderPath,
                       kU_CoTaskMemFree,
                       false,
                       lblError);

    if (!layout.compressedPayloadRelativePath.empty()) {
        emitRunPowerShellBackend(gen,
                                 layout,
                                 "uninstall-files",
                                 kU_lstrcpyW,
                                 kU_lstrcatW,
                                 kU_lstrlenW,
                                 kU_CloseHandle,
                                 kU_SetEnvironmentVariableW,
                                 kU_GetSystemDirectoryW,
                                 kU_CreateProcessW,
                                 kU_WaitForSingleObject,
                                 kU_GetExitCodeProcess,
                                 false,
                                 lblError);
    }

    if (layout.compressedPayloadRelativePath.empty()) {
        for (const auto &file : layout.uninstallFiles) {
            emitDeleteFile(gen,
                           file,
                           slashOff,
                           kU_lstrcpyW,
                           kU_lstrcatW,
                           kU_lstrlenW,
                           kU_DeleteFileW,
                           kU_GetLastError,
                           lblError);
        }
    }

    const auto lblSkipSelfDelete = gen.newLabel();
    gen.movRegMem(X64Reg::RAX, X64Reg::RBP, kNoRestartModeOff);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblSkipSelfDelete);
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kSelfPathOff);
    gen.xorRegReg(X64Reg::RDX, X64Reg::RDX);
    gen.movRegImm32(X64Reg::R8, kMoveFileDelayUntilReboot);
    gen.callIATSlot(kU_MoveFileExW);
    gen.bindLabel(lblSkipSelfDelete);

    for (const auto &dir : layout.uninstallDirectories) {
        emitRemoveDirectory(gen,
                            dir,
                            slashOff,
                            kU_lstrcpyW,
                            kU_lstrcatW,
                            kU_lstrlenW,
                            kU_RemoveDirectoryW,
                            kU_GetLastError,
                            lblError);
    }

    if (needsMenuPath(layout)) {
        const auto lblMenuPathDone = gen.newLabel();
        gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kMenuPathOff);
        gen.callIATSlot(kU_RemoveDirectoryW);
        gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
        gen.jnz(lblMenuPathDone);
        gen.callIATSlot(kU_GetLastError);
        gen.cmpRegImm32(X64Reg::RAX, kErrorFileNotFound);
        gen.jz(lblMenuPathDone);
        gen.cmpRegImm32(X64Reg::RAX, kErrorPathNotFound);
        gen.jz(lblMenuPathDone);
        gen.cmpRegImm32(X64Reg::RAX, kErrorDirNotEmpty);
        gen.jz(lblMenuPathDone);
        gen.jmp(lblError);
        gen.bindLabel(lblMenuPathDone);
    }

    const auto lblInstallPathDone = gen.newLabel();
    gen.leaRegMem(X64Reg::RCX, X64Reg::RBP, kInstallPathOff);
    gen.callIATSlot(kU_RemoveDirectoryW);
    gen.testRegReg(X64Reg::RAX, X64Reg::RAX);
    gen.jnz(lblInstallPathDone);
    gen.callIATSlot(kU_GetLastError);
    gen.cmpRegImm32(X64Reg::RAX, kErrorFileNotFound);
    gen.jz(lblInstallPathDone);
    gen.cmpRegImm32(X64Reg::RAX, kErrorPathNotFound);
    gen.jz(lblInstallPathDone);
    gen.cmpRegImm32(X64Reg::RAX, kErrorDirNotEmpty);
    gen.jz(lblInstallPathDone);
    gen.jmp(lblError);
    gen.bindLabel(lblInstallPathDone);

    emitUnregisterFileAssociations(gen,
                                   layout,
                                   kU_RegOpenKeyW,
                                   kU_RegCloseKey,
                                   kU_RegQueryValueExW,
                                   kU_lstrcmpW,
                                   kU_RegDeleteValueW,
                                   kU_RegDeleteTreeW);

    emitRestorePathFromUninstallKey(gen,
                                    layout,
                                    slashOff,
                                    semicolonOff,
                                    uninstallKeyOff,
                                    environmentKeyOff,
                                    regPathValueOff,
                                    regPathEntryOff,
                                    environmentOff,
                                    kU_RegOpenKeyW,
                                    kU_RegCreateKeyW,
                                    kU_RegSetValueExW,
                                    kU_RegQueryValueExW,
                                    kU_RegCloseKey,
                                    kU_lstrcpyW,
                                    kU_lstrcatW,
                                    kU_lstrcmpiW,
                                    kU_lstrlenW,
                                    kU_SendMessageTimeoutW,
                                    lblError);

    gen.movRegImm64(X64Reg::RCX, registryRoot);
    gen.leaRipData(X64Reg::RDX, uninstallKeyOff);
    gen.callIATSlot(kU_RegDeleteTreeW);

    emitMessageBoxUnlessQuiet(gen, kU_MessageBoxW, successTitleOff, successMsgOff, 0x40);
    gen.jmp(lblExitSuccess);

    gen.bindLabel(lblError);
    emitMessageBoxUnlessQuiet(gen, kU_MessageBoxW, errorTitleOff, errorMsgOff, 0x10);
    gen.jmp(lblExitError);

    gen.bindLabel(lblExitSuccess);
    gen.xorRegReg(X64Reg::RCX, X64Reg::RCX);
    gen.callIATSlot(kU_ExitProcess);

    gen.bindLabel(lblExitCancelled);
    gen.movRegImm32(X64Reg::RCX, kErrorInstallUserExit);
    gen.callIATSlot(kU_ExitProcess);

    gen.bindLabel(lblExitError);
    gen.movRegImm32(X64Reg::RCX, 1);
    gen.callIATSlot(kU_ExitProcess);

    emitWizardDialogProc(gen,
                         layout,
                         lblWizardDialogProc,
                         kU_EndDialog,
                         kU_GetDlgItem,
                         kU_SendMessageW,
                         kU_EnableWindow,
                         darkThemeOff,
                         kU_GetDlgCtrlID,
                         kU_SetWindowTheme,
                         kU_DwmSetWindowAttribute,
                         kU_GetStockObject,
                         kU_SetDCBrushColor,
                         kU_SetTextColor,
                         kU_SetBkColor);

    finalizeStubRVAs(result, gen);
    result.stubData = gen.dataSection();
    return result;
}

} // namespace zanna::pkg
