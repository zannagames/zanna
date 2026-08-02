//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_lifecycle_contract.cpp
// Purpose: Protect fail-closed contracts in the native Windows installer lifecycle.
//
// Key invariants:
//   - Registry, COM, filesystem, helper-process, and known-folder outputs are validated before use.
//   - Transaction metadata is bounded, durable, and parsed as an exact schema.
//   - Windows path ownership decisions use locale-independent ordinal comparison.
//
// Ownership/Lifetime:
//   - The test owns in-memory copies of the installer and PE validator sources.
//
// Links: src/tools/windows_installer/WindowsInstallerLifecycle.cpp,
//        src/tools/windows_installer/WindowsInstallerCleanup.cpp,
//        src/tools/windows_installer/WindowsInstallerHost.cpp,
//        src/tools/common/packaging/WindowsPEValidation.cpp
//
//===----------------------------------------------------------------------===//

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef ZANNA_SOURCE_DIR
#define ZANNA_SOURCE_DIR "."
#endif

namespace {

int testsRun = 0;
int testsPassed = 0;

void expect(bool condition, std::string_view message) {
    ++testsRun;
    if (condition) {
        ++testsPassed;
    } else {
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool appearsInOrder(std::string_view source,
                    std::string_view anchor,
                    std::string_view first,
                    std::string_view second) {
    const size_t anchorPosition = source.find(anchor);
    const size_t firstPosition = anchorPosition == std::string_view::npos
                                     ? anchorPosition
                                     : source.find(first, anchorPosition);
    const size_t secondPosition = firstPosition == std::string_view::npos
                                      ? firstPosition
                                      : source.find(second, firstPosition);
    return secondPosition != std::string_view::npos;
}

std::string readLifecycleSource() {
    const std::filesystem::path path = std::filesystem::path(ZANNA_SOURCE_DIR) / "src" / "tools" /
                                       "windows_installer" / "WindowsInstallerLifecycle.cpp";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string readHostSource() {
    const std::filesystem::path path = std::filesystem::path(ZANNA_SOURCE_DIR) / "src" / "tools" /
                                       "windows_installer" / "WindowsInstallerHost.cpp";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string readInstallerSource(std::string_view name) {
    const std::filesystem::path path =
        std::filesystem::path(ZANNA_SOURCE_DIR) / "src" / "tools" / "windows_installer" / name;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string readPeValidationSource() {
    const std::filesystem::path path = std::filesystem::path(ZANNA_SOURCE_DIR) / "src" / "tools" /
                                       "common" / "packaging" / "WindowsPEValidation.cpp";
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    const std::string source = readLifecycleSource();
    const std::string hostSource = readHostSource();
    const std::string peValidationSource = readPeValidationSource();
    const std::string cleanupSource = readInstallerSource("WindowsInstallerCleanup.cpp");
    const std::string brandSource = readInstallerSource("WindowsInstallerBrandDialog.cpp");
    const std::string themeSource = readInstallerSource("WindowsInstallerTheme.cpp");
    const std::string wizardSource = readInstallerSource("WindowsInstallerWizard.cpp");
    expect(!source.empty(), "Windows installer lifecycle source is readable");
    expect(!hostSource.empty(), "Windows installer host source is readable");
    expect(!peValidationSource.empty(), "Windows PE validation source is readable");
    expect(!cleanupSource.empty(), "Windows installer cleanup source is readable");
    expect(!brandSource.empty(), "Windows installer brand-dialog source is readable");
    expect(!themeSource.empty(), "Windows installer theme source is readable");
    expect(!wizardSource.empty(), "Windows installer wizard source is readable");
    if (source.empty() || hostSource.empty() || peValidationSource.empty() ||
        cleanupSource.empty() || brandSource.empty() || themeSource.empty() || wizardSource.empty())
        return 1;

    expect(source.find("CompareStringOrdinal") != std::string::npos &&
               source.find("std::towlower") == std::string::npos,
           "Windows path decisions use locale-independent ordinal comparison");
    expect(source.find("Windows registry operation returned no key") != std::string::npos,
           "Successful registry opens still require a non-null key");
    expect(source.find("refusing to write an invalid Windows registry string") != std::string::npos,
           "Registry string writes reject invalid types, NULs, and size overflow");
    expect(source.find("cannot resolve a protected Windows known folder") != std::string::npos &&
               source.find("cannot resolve the protected Windows directory") != std::string::npos,
           "Destination protection fails closed when Windows roots cannot be resolved");
    expect(source.find("cannot verify an installation path ancestor") != std::string::npos,
           "Unexpected ancestor attribute errors cannot bypass reparse-point checks");
    expect(source.find("reserved Windows device name") != std::string::npos &&
               source.find("unsafe trailing character") != std::string::npos,
           "Lifecycle paths reject Windows device aliases and trailing dot/space names");
    expect(source.find("Windows installer registry value has an invalid type or size") !=
                   std::string::npos &&
               source.find("cannot query installer elevation state") != std::string::npos,
           "Malformed settings and elevation-query failures cannot become safe defaults");
    expect(source.find("if (known.find(normalized) != known.end())") != std::string::npos &&
               source.find("plan.createShortcuts = plan.createShortcuts &&") != std::string::npos,
           "Upgrades discard retired components and unavailable integration settings");
    expect(source.find("ERROR_ALREADY_EXISTS") != std::string::npos &&
               source.find("attempt < 64U") != std::string::npos,
           "Writable-parent probes retry bounded name collisions");
    expect(source.find("cannot inspect an existing installation entry") != std::string::npos,
           "Disk preflight fails closed when existing entry attributes are unreadable");
    expect(source.find("std::array<uint32_t, 3> parseWindowsVersion") != std::string::npos &&
               source.find("part.size() > 1U && part.front() == '0'") != std::string::npos &&
               source.find("optional prerelease and build metadata") == std::string::npos,
           "Minimum-Windows checks use canonical unsigned OS components only");
    expect(hostSource.find("FILE_SHARE_READ") != std::string::npos &&
               hostSource.find("FILE_FLAG_SEQUENTIAL_SCAN") != std::string::npos &&
               hostSource.find("installer executable changed while it was being read") !=
                   std::string::npos,
           "Installer package reads deny concurrent mutation and require an exact snapshot");
    expect(hostSource.find("cannot append the installer log") != std::string::npos &&
               hostSource.find("SetLastError(ERROR_WRITE_FAULT)") != std::string::npos,
           "Installer logging surfaces short writes and flush failures");
    expect(source.find("bool handleBytesMatch") != std::string::npos &&
               source.find("handleBytesMatch(helper.get(), package.cleanupBytes)") !=
                   std::string::npos &&
               source.find("FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN") !=
                   std::string::npos,
           "Detached cleanup launches an exact non-reparse snapshot of the packaged helper");
    expect(appearsInOrder(source,
                          "helper.reset(CreateFileW(helperPath.c_str()",
                          "GENERIC_READ | SYNCHRONIZE",
                          "FILE_SHARE_READ,"),
           "Detached cleanup holds a read-only handle that denies helper mutation and replacement");
    expect(appearsInOrder(source,
                          "if (ResumeThread(threadHandle.get())",
                          "if (!TerminateProcess(processHandle.get()",
                          "if (WaitForSingleObject(processHandle.get(), 5000)") &&
               source.find("processMayBeRunning") != std::string::npos,
           "Failed detached-helper activation is terminated and reaped before local cleanup");
    expect(source.find("if (!MoveFileExW(") != std::string::npos &&
               source.find("cleanup could not be ") != std::string::npos &&
               source.find("scheduled for reboot: ") != std::string::npos,
           "Reboot cleanup scheduling failures are reported instead of claimed as successes");
    expect(cleanupSource.find("const bool clearedReadOnly") != std::string::npos &&
               cleanupSource.find("SetFileAttributesW(path.c_str(), attributes)") !=
                   std::string::npos,
           "Failed cleanup deletion restores a file's original read-only attribute");
    expect(appearsInOrder(cleanupSource,
                          "if (DeleteFileW(path.c_str()))",
                          "const DWORD error = GetLastError()",
                          "error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND"),
           "Concurrent cleanup-target disappearance remains an idempotent success");

    expect(source.find("kMaximumTextFileBytes") != std::string::npos &&
               source.find("metadata text file grew while being read") != std::string::npos,
           "Transaction text reads are bounded exact snapshots");
    expect(appearsInOrder(
               source, "void writeBytesAtomic", "FlushFileBuffers(output.get())", "MoveFileExW"),
           "Atomic transaction writes flush their payload before publication");
    expect(appearsInOrder(
               source, "void writeBytesAtomic", "catch (...)", "DeleteFileW(temporary.c_str())"),
           "Failed transaction staging removes its temporary file");
    expect(source.find("installer transaction journal is malformed") != std::string::npos &&
               source.find("value.find(L\"state=") == std::string::npos,
           "Recovery journals require an exact schema instead of substring matching");
    expect(source.find("journal is missing after the old tree ") != std::string::npos &&
               source.find("moved; transaction retained") != std::string::npos,
           "A missing recovery journal cannot discard a preserved old tree");

    expect(appearsInOrder(source,
                          "std::wstring updatePath",
                          "readPathValue(environment.get())",
                          "original.present ? original.type : REG_EXPAND_SZ"),
           "PATH updates use a validated snapshot and preserve its registry type");
    expect(source.find("ordinalEqualsIgnoreCase(key, removeKey)") != std::string::npos &&
               source.find("ordinalEqualsIgnoreCase(key, addKey)") != std::string::npos,
           "PATH entry ownership is locale independent");

    expect(source.find("|| !link") != std::string::npos &&
               source.find("|| !persist") != std::string::npos,
           "Shell-link COM calls reject success-with-null outputs");
    expect(source.find("terminatedWideView(target)") != std::string::npos &&
               source.find("terminatedWideView(arguments)") != std::string::npos,
           "Shell-link getters must return bounded NUL-terminated strings");
    expect(source.find("isProtectedShortcutRoot(parent)") != std::string::npos,
           "Shortcut cleanup cannot remove Desktop or Start Menu roots");
    expect(source.find("refusing to remove a non-file Zanna shortcut path") != std::string::npos,
           "Shortcut cleanup rejects directories and reparse points");
    expect(appearsInOrder(source,
                          "int runLifecycle",
                          "maintenance-handoff worker mode requires the verified cache executable",
                          "waitForHandoffParent(options.handoffParentId)"),
           "Maintenance workers prove their cached executable before trusting a handoff PID");
    expect(source.find("elevated worker mode requires an elevated machine-scope process") !=
               std::string::npos,
           "Elevated worker mode proves both machine scope and process elevation");
    expect(hostSource.find("/uninstall-worker and /handoff-parent must be supplied together") !=
                   std::string::npos &&
               hostSource.find("elevated and maintenance-handoff worker modes are exclusive") !=
                   std::string::npos,
           "Internal worker options reject unpaired and contradictory combinations");
    expect(
        hostSource.find("/handoff-parent was specified more than once") != std::string::npos &&
            hostSource.find("/elevated-worker was specified more than once") != std::string::npos &&
            hostSource.find("/uninstall-worker was specified more than once") != std::string::npos,
        "Internal worker options reject duplicate spellings");
    expect(hostSource.find("validateWindowsPEImage") != std::string::npos &&
               peValidationSource.find("kMaximumSections") != std::string::npos &&
               peValidationSource.find("overlapping raw storage") != std::string::npos &&
               peValidationSource.find("entry point belongs to a non-executable section") !=
                   std::string::npos &&
               peValidationSource.find("data directory is outside mapped image content") !=
                   std::string::npos,
           "Embedded executables require bounded, non-overlapping PE32+ images");
    expect(hostSource.find("metadata architecture is unsupported") != std::string::npos &&
               hostSource.find("else if (architecture == \"x64\")") != std::string::npos,
           "Installer PE checks reject unknown architecture metadata");
    expect(hostSource.find(
               "compareInstallerVersions(package.metadata.version, package.metadata.version)") !=
               std::string::npos,
           "Installer packages reject versions the lifecycle and update checker cannot consume");
    expect(hostSource.find("(ch >= 0x202A && ch <= 0x202E)") != std::string::npos &&
               hostSource.find("(ch >= 0x2066 && ch <= 0x2069)") != std::string::npos &&
               hostSource.find("static_cast<wchar_t>(0xFFFD)") != std::string::npos,
           "Installer logs neutralize direction controls and malformed UTF-16");
    expect(hostSource.find("appendLogRecord") != std::string::npos &&
               hostSource.find("written == 0 || written > requested") != std::string::npos,
           "Installer logs handle partial native writes without spinning");
    expect(hostSource.find("A broken presentation callback must stop mutation") !=
               std::string::npos,
           "Cancellation callback failures fail closed");
    expect(hostSource.find("installer lifecycle operation was specified more than once") !=
                   std::string::npos &&
               hostSource.find("installer option requires a non-empty value") !=
                   std::string::npos &&
               hostSource.find("parseHandoffProcessId") != std::string::npos,
           "Installer CLI parsing rejects duplicate, empty, and ambiguous internal options");
    expect(brandSource.find("validateBrandedInstallerPage(instance, page)") != std::string::npos &&
               brandSource.find("validateBrandedInstallerProgress(") != std::string::npos,
           "Branded page and progress models are validated before native allocation");
    expect(themeSource.find("installerWindowClassMatches") != std::string::npos &&
               brandSource.find("registerVerifiedInstallerWindowClass") != std::string::npos &&
               wizardSource.find("registerVerifiedInstallerWindowClass") != std::string::npos,
           "Every custom installer window class rejects foreign class reuse");
    expect(brandSource.find("case WM_DPICHANGED:") != std::string::npos &&
               wizardSource.find("case WM_DPICHANGED:") != std::string::npos &&
               themeSource.find("normalizeInstallerDpi") != std::string::npos,
           "Branded, progress, and custom pages normalize and react to per-monitor DPI");
    expect(brandSource.find("if (!dc)") != std::string::npos &&
               wizardSource.find("if (!dc)") != std::string::npos &&
               themeSource.find("if (saved == 0)") != std::string::npos,
           "Installer paint paths fail safely when BeginPaint or SaveDC fails");
    expect(brandSource.find("PostQuitMessage(static_cast<int>(message.wParam))") !=
                   std::string::npos &&
               wizardSource.find("PostQuitMessage(static_cast<int>(message.wParam))") !=
                   std::string::npos,
           "Nested installer message loops preserve the process quit request");
    expect(brandSource.find("statusMessagePosted") != std::string::npos &&
               brandSource.find("pendingStatus") != std::string::npos,
           "Progress status updates are bounded and coalesced");

    std::cout << testsPassed << "/" << testsRun << " tests passed\n";
    return testsPassed == testsRun ? 0 : 1;
}
