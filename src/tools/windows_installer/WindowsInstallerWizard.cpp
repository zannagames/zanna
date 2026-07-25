//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerWizard.cpp
// Purpose: Implement the native, accessible Windows installer configuration,
//          progress, and successful-completion experience.
//
// Key invariants:
//   - Native controls preserve system high-contrast, keyboard, screen-reader,
//     and per-monitor DPI behavior.
//   - Component and destination choices are collected before lifecycle writes.
//   - Progress work executes off the UI thread and propagates its exact result.
//   - The progress surface cannot close while a transaction is in flight.
//
// Ownership/Lifetime:
//   - Modal contexts outlive their HWNDs and worker threads are joined.
//
// Links: WindowsInstallerWizard.hpp, WindowsInstallerBrandDialog.cpp,
//        WindowsInstallerTheme.cpp, WindowsInstallerLifecycle.cpp,
//        docs/adr/0175-zanna-games-windows-installer-experience.md
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerWizard.hpp"
#include "WindowsInstallerBrandDialog.hpp"
#include "WindowsInstallerResources.h"
#include "WindowsInstallerTheme.hpp"
#include "WindowsInstallerUpdate.hpp"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <exception>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace zanna::installer {
namespace {

constexpr int kIdUserScope = 1101;
constexpr int kIdMachineScope = 1102;
constexpr int kIdDestination = 1103;
constexpr int kIdBrowse = 1104;
constexpr int kIdMinimal = 1105;
constexpr int kIdTypical = 1106;
constexpr int kIdComplete = 1107;
constexpr int kIdSDK = 1111;
constexpr int kIdPath = 1108;
constexpr int kIdAssociations = 1109;
constexpr int kIdShortcuts = 1110;
constexpr int kIdFirstComponent = 1200;
constexpr int kIdAccept = IDOK;
constexpr int kMaximumNativePathUnits = 32767;

constexpr int kWelcomeRecommended = 2001;
constexpr int kWelcomeComplete = 2002;
constexpr int kWelcomeCustom = 2003;
constexpr int kWelcomeSDK = 2004;
constexpr int kWelcomeUpdate = 2005;
constexpr int kMaintenanceModify = 2011;
constexpr int kMaintenanceRepair = 2012;
constexpr int kMaintenanceRemove = 2013;
constexpr int kMaintenanceUpdate = 2014;
constexpr int kReadyContinue = 2021;
constexpr int kReadyBack = 2022;
constexpr int kFinishLaunch = 2031;
constexpr int kFinishPrompt = 2032;
constexpr int kFinishQuickstart = 2033;
constexpr int kFinishClose = 2034;
constexpr int kFinishSamples = 2035;
constexpr int kFinishCopyVerification = 2036;

std::wstring formatBytes(uint64_t bytes) {
    static constexpr std::array<const wchar_t *, 5> kUnits = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
    long double value = static_cast<long double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0L && unit + 1U < kUnits.size()) {
        value /= 1024.0L;
        ++unit;
    }
    std::wostringstream out;
    if (unit == 0)
        out << static_cast<uint64_t>(value);
    else
        out << std::fixed << std::setprecision(value < 10.0L ? 1 : 0) << value;
    out << L' ' << kUnits[unit];
    return out.str();
}

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const HRESULT folderResult = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value);
    if (FAILED(folderResult) || !value) {
        if (value)
            CoTaskMemFree(value);
        throw std::runtime_error("cannot resolve a Windows known folder for setup");
    }
    std::wstring result(value);
    CoTaskMemFree(value);
    return result;
}

fs::path defaultDestination(const HostPackage &package, InstallScope scope) {
    const fs::path base = scope == InstallScope::User
                              ? fs::path(knownFolder(FOLDERID_LocalAppData)) / L"Programs"
                              : fs::path(knownFolder(FOLDERID_ProgramFiles));
    return base / utf8ToWide(package.metadata.defaultInstallDir);
}

std::optional<fs::path> environmentFolder(const wchar_t *name) {
    DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
    if (capacity == 0U)
        return std::nullopt;
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        std::wstring value(capacity, L'\0');
        const DWORD length = GetEnvironmentVariableW(name, value.data(), capacity);
        if (length > 0U && length < capacity) {
            value.resize(length);
            return fs::path(value);
        }
        if (length == 0U || length == std::numeric_limits<DWORD>::max())
            return std::nullopt;
        capacity = length + 1U;
    }
    return std::nullopt;
}

bool ordinaryFile(const fs::path &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U;
}

bool commandAvailable(const wchar_t *command) {
    wchar_t path[32768]{};
    const DWORD found =
        SearchPathW(nullptr, command, nullptr, static_cast<DWORD>(std::size(path)), path, nullptr);
    return found > 0U && found < std::size(path);
}

std::vector<fs::path> visualStudioInstallations() {
    std::vector<fs::path> results;
    for (const wchar_t *variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const auto programFiles = environmentFolder(variable);
        if (!programFiles)
            continue;
        const fs::path root = *programFiles / L"Microsoft Visual Studio";
        std::error_code error;
        for (fs::directory_iterator year(root, error); !error && year != fs::directory_iterator();
             year.increment(error)) {
            if (!year->is_directory(error)) {
                error.clear();
                continue;
            }
            for (fs::directory_iterator edition(year->path(), error);
                 !error && edition != fs::directory_iterator();
                 edition.increment(error)) {
                if (edition->is_directory(error))
                    results.push_back(edition->path());
                error.clear();
            }
            error.clear();
        }
    }
    return results;
}

bool visualCppAvailable(const std::vector<fs::path> &visualStudios) {
    if (commandAvailable(L"cl.exe"))
        return true;
    for (const fs::path &installation : visualStudios) {
        if (ordinaryFile(installation / L"VC" / L"Auxiliary" / L"Build" / L"vcvarsall.bat"))
            return true;
    }
    return false;
}

bool windowsSdkAvailable() {
    if (commandAvailable(L"rc.exe"))
        return true;
    for (const wchar_t *variable : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        const auto programFiles = environmentFolder(variable);
        if (!programFiles)
            continue;
        const fs::path includeRoot = *programFiles / L"Windows Kits" / L"10" / L"Include";
        std::error_code error;
        for (fs::directory_iterator version(includeRoot, error);
             !error && version != fs::directory_iterator();
             version.increment(error)) {
            if (ordinaryFile(version->path() / L"um" / L"Windows.h"))
                return true;
            error.clear();
        }
    }
    return false;
}

bool gitAvailable() {
    if (commandAvailable(L"git.exe"))
        return true;
    for (const wchar_t *variable : {L"ProgramFiles", L"LocalAppData"}) {
        const auto root = environmentFolder(variable);
        if (root && (ordinaryFile(*root / L"Git" / L"cmd" / L"git.exe") ||
                     ordinaryFile(*root / L"Programs" / L"Git" / L"cmd" / L"git.exe")))
            return true;
    }
    return false;
}

bool cmakeAvailable() {
    if (commandAvailable(L"cmake.exe"))
        return true;
    const auto programFiles = environmentFolder(L"ProgramFiles");
    return programFiles && ordinaryFile(*programFiles / L"CMake" / L"bin" / L"cmake.exe");
}

bool ninjaAvailable(const std::vector<fs::path> &visualStudios) {
    if (commandAvailable(L"ninja.exe"))
        return true;
    return std::any_of(
        visualStudios.begin(), visualStudios.end(), [](const fs::path &installation) {
            return ordinaryFile(installation / L"Common7" / L"IDE" / L"CommonExtensions" /
                                L"Microsoft" / L"CMake" / L"Ninja" / L"ninja.exe");
        });
}

bool visualStudioCodeAvailable() {
    if (commandAvailable(L"code.cmd") || commandAvailable(L"code.exe"))
        return true;
    for (const wchar_t *variable : {L"LocalAppData", L"ProgramFiles"}) {
        const auto root = environmentFolder(variable);
        if (!root)
            continue;
        const fs::path base = std::wcscmp(variable, L"LocalAppData") == 0
                                  ? *root / L"Programs" / L"Microsoft VS Code"
                                  : *root / L"Microsoft VS Code";
        if (ordinaryFile(base / L"Code.exe") || ordinaryFile(base / L"bin" / L"code.cmd"))
            return true;
    }
    return false;
}

bool windowsTerminalAvailable() {
    if (commandAvailable(L"wt.exe"))
        return true;
    const auto local = environmentFolder(L"LocalAppData");
    return local && ordinaryFile(*local / L"Microsoft" / L"WindowsApps" / L"wt.exe");
}

std::wstring dependencySummary() {
    struct Dependency {
        const wchar_t *label;
        bool available;
    };

    const std::vector<fs::path> visualStudios = visualStudioInstallations();
    const std::array<Dependency, 7> dependencies = {
        {{L"Git", gitAvailable()},
         {L"CMake", cmakeAvailable()},
         {L"Ninja", ninjaAvailable(visualStudios)},
         {L"Visual Studio C++", visualCppAvailable(visualStudios)},
         {L"Windows SDK", windowsSdkAvailable()},
         {L"VS Code", visualStudioCodeAvailable()},
         {L"Windows Terminal", windowsTerminalAvailable()}}};
    std::wstring result = L"Optional developer companions detected:\r\n";
    for (const Dependency &dependency : dependencies) {
        result += dependency.available ? L"  \x2713 " : L"  \x2014 ";
        result += dependency.label;
        result += dependency.available ? L" detected\r\n" : L" not detected\r\n";
    }
    result += L"\r\nZanna itself is self-contained. Setup never downloads optional companions.";
    return result;
}

void copyTextToClipboard(std::wstring_view text) {
    if (text.size() > std::numeric_limits<size_t>::max() / sizeof(wchar_t) - 1U)
        throw std::runtime_error("clipboard text is too large");
    const size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory)
        throw std::runtime_error("cannot allocate clipboard text");
    void *destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        throw std::runtime_error("cannot access clipboard text");
    }
    std::memcpy(destination, text.data(), text.size() * sizeof(wchar_t));
    GlobalUnlock(memory);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        throw std::runtime_error("cannot open the Windows clipboard");
    }

    struct ClipboardGuard {
        ~ClipboardGuard() {
            CloseClipboard();
        }
    } guard;

    if (!EmptyClipboard()) {
        GlobalFree(memory);
        throw std::runtime_error("cannot clear the Windows clipboard");
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        throw std::runtime_error("cannot publish clipboard text");
    }
}

void checkUpdatesInteractive(HINSTANCE instance, const HostPackage &package) {
    try {
        showUpdateResult(instance, package, checkForUpdates(package));
    } catch (const std::exception &error) {
        const std::wstring message = L"Zanna could not check for updates.\r\n\r\n" +
                                     utf8ToWide(error.what()) +
                                     L"\r\n\r\nNo files were downloaded or changed.";
        MessageBoxW(nullptr,
                    message.c_str(),
                    L"Zanna Update Check",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

struct ComponentControl {
    const zanna::pkg::WindowsInstallerComponentMetadata *metadata{nullptr};
    HWND checkbox{nullptr};
};

struct CustomDialogContext {
    HINSTANCE instance{nullptr};
    const HostPackage *package{nullptr};
    HostOptions *options{nullptr};
    fs::path initialDestination;
    InstallScope initialScope{InstallScope::User};
    std::set<std::string> initialComponents;
    std::vector<ComponentControl> components;
    HWND window{nullptr};
    HWND destination{nullptr};
    HWND associationOption{nullptr};
    HWND acceptButton{nullptr};
    std::unique_ptr<InstallerThemeResources> theme;
    bool scopeLocked{false};
    bool accepted{false};
    InstallScope displayedScope{InstallScope::User};
    int virtualWidth{0};
    int virtualHeight{0};
    int wheelDelta{0};
    int defaultButtonId{kIdAccept};
};

int scaled(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int consumeOptionsWheelSteps(WPARAM value, int &remainder) noexcept {
    remainder += GET_WHEEL_DELTA_WPARAM(value);
    const int steps = remainder / WHEEL_DELTA;
    remainder -= steps * WHEEL_DELTA;
    return steps;
}

void applyOptionsDpiBounds(HWND window, const RECT *bounds) noexcept {
    if (!window || !bounds || bounds->right <= bounds->left || bounds->bottom <= bounds->top)
        return;
    SetWindowPos(window,
                 nullptr,
                 bounds->left,
                 bounds->top,
                 bounds->right - bounds->left,
                 bounds->bottom - bounds->top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

void setControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND createControl(CustomDialogContext &context,
                   DWORD exStyle,
                   const wchar_t *className,
                   const wchar_t *text,
                   DWORD style,
                   int x,
                   int y,
                   int width,
                   int height,
                   int id,
                   UINT dpi) {
    HWND control = CreateWindowExW(exStyle,
                                   className,
                                   text,
                                   style | WS_CHILD | WS_VISIBLE,
                                   scaled(x, dpi),
                                   scaled(y, dpi),
                                   scaled(width, dpi),
                                   scaled(height, dpi),
                                   context.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   context.instance,
                                   nullptr);
    if (!control)
        throw std::runtime_error("cannot create a native setup control");
    setControlFont(control, context.theme->bodyFont());
    applyInstallerControlTheme(control, *context.theme);
    return control;
}

void updateOptionsScrollbars(CustomDialogContext &context);

void updateOptionsDpi(CustomDialogContext &context,
                      UINT requestedDpi,
                      const RECT *suggestedBounds) noexcept {
    const UINT newDpi = normalizeInstallerDpi(requestedDpi);
    const UINT oldDpi = context.theme->dpi();
    try {
        auto replacement = std::make_unique<InstallerThemeResources>(newDpi);
        if (oldDpi != newDpi) {
            (void)rescaleInstallerChildWindows(context.window, oldDpi, newDpi);
            context.virtualWidth =
                MulDiv(context.virtualWidth, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.virtualHeight =
                MulDiv(context.virtualHeight, static_cast<int>(newDpi), static_cast<int>(oldDpi));
        }
        context.theme = std::move(replacement);
        applyInstallerWindowTheme(context.window, *context.theme);
        for (HWND child = GetWindow(context.window, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            setControlFont(child, context.theme->bodyFont());
            applyInstallerControlTheme(child, *context.theme);
        }
    } catch (...) {
        return;
    }
    applyOptionsDpiBounds(context.window, suggestedBounds);
    updateOptionsScrollbars(context);
    InvalidateRect(context.window, nullptr, TRUE);
}

std::wstring readWindowTextExact(HWND window) {
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        const int length = GetWindowTextLengthW(window);
        if (length < 0 || (length == 0 && GetLastError() != ERROR_SUCCESS))
            break;
        if (length > kMaximumNativePathUnits)
            throw std::runtime_error("installation folder is too long");
        std::wstring text(static_cast<size_t>(length) + 1U, L'\0');
        SetLastError(ERROR_SUCCESS);
        const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        if (copied < 0 || (copied == 0 && (length > 0 || GetLastError() != ERROR_SUCCESS)))
            break;
        SetLastError(ERROR_SUCCESS);
        const int verifiedLength = GetWindowTextLengthW(window);
        if (verifiedLength < 0 || (verifiedLength == 0 && GetLastError() != ERROR_SUCCESS)) {
            break;
        }
        if (verifiedLength <= copied) {
            text.resize(static_cast<size_t>(copied));
            return text;
        }
    }
    throw std::runtime_error("cannot read the current installation folder");
}

void browseForDestination(CustomDialogContext &context) {
    const std::wstring current = readWindowTextExact(context.destination);
    IFileOpenDialog *dialog = nullptr;
    const HRESULT createResult = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(createResult) || !dialog) {
        if (dialog)
            dialog->Release();
        throw std::runtime_error("cannot create the Windows folder picker");
    }

    struct DialogRelease {
        IFileOpenDialog *value;

        ~DialogRelease() {
            if (value)
                value->Release();
        }
    } release{dialog};

    FILEOPENDIALOGOPTIONS flags{};
    if (FAILED(dialog->GetOptions(&flags)) ||
        FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                  FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT)) ||
        FAILED(dialog->SetTitle(L"Choose the Zanna installation folder"))) {
        throw std::runtime_error("cannot configure the Windows folder picker");
    }
    fs::path initial(current);
    std::error_code error;
    while (!initial.empty() && !fs::is_directory(initial, error)) {
        error.clear();
        initial = initial.parent_path();
    }
    if (!initial.empty()) {
        IShellItem *folder = nullptr;
        const HRESULT folderResult =
            SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(folderResult) && folder) {
            const HRESULT setResult = dialog->SetFolder(folder);
            folder->Release();
            if (FAILED(setResult))
                throw std::runtime_error("cannot set the initial installation folder");
        } else if (folder) {
            folder->Release();
        }
    }
    const HRESULT shown = dialog->Show(context.window);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(shown))
        throw std::runtime_error("the Windows folder picker failed");
    IShellItem *selected = nullptr;
    const HRESULT result = dialog->GetResult(&selected);
    if (FAILED(result) || !selected) {
        if (selected)
            selected->Release();
        throw std::runtime_error("cannot read the selected installation folder");
    }

    struct ItemRelease {
        IShellItem *value;

        ~ItemRelease() {
            if (value)
                value->Release();
        }
    } itemRelease{selected};

    PWSTR path = nullptr;
    const HRESULT pathResult = selected->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(pathResult) || !path) {
        if (path)
            CoTaskMemFree(path);
        throw std::runtime_error("the selected item is not a local filesystem folder");
    }
    if (!SetWindowTextW(context.destination, path)) {
        CoTaskMemFree(path);
        throw std::runtime_error("cannot update the selected installation folder");
    }
    CoTaskMemFree(path);
}

void updateAssociationControl(CustomDialogContext &context) {
    if (!context.associationOption)
        return;
    bool executableSelected = true;
    const std::string associationPath = context.package->metadata.associationExecutable;
    const auto payload = std::find_if(context.package->metadata.payloadFiles.begin(),
                                      context.package->metadata.payloadFiles.end(),
                                      [&](const zanna::pkg::WindowsInstallerPayloadMetadata &file) {
                                          return file.path == associationPath;
                                      });
    if (payload != context.package->metadata.payloadFiles.end() && !payload->componentId.empty()) {
        const auto control = std::find_if(context.components.begin(),
                                          context.components.end(),
                                          [&](const ComponentControl &component) {
                                              return component.metadata->id == payload->componentId;
                                          });
        executableSelected = control != context.components.end() &&
                             SendMessageW(control->checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    const bool available = executableSelected && !context.package->metadata.associations.empty();
    EnableWindow(context.associationOption, available);
    if (!available)
        SendMessageW(context.associationOption, BM_SETCHECK, BST_UNCHECKED, 0);
}

void selectPreset(CustomDialogContext &context, ComponentPreset preset) {
    for (const ComponentControl &component : context.components) {
        const auto &metadata = *component.metadata;
        bool checked = metadata.required;
        if (preset == ComponentPreset::Typical)
            checked = checked || metadata.defaultSelected;
        else if (preset == ComponentPreset::SDK)
            checked = checked || metadata.id == "sdk";
        else if (preset == ComponentPreset::Complete)
            checked = true;
        SendMessageW(component.checkbox, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    updateAssociationControl(context);
}

void acceptCustomDialog(CustomDialogContext &context) {
    const std::wstring destination = readWindowTextExact(context.destination);
    if (destination.empty()) {
        MessageBoxW(context.window,
                    L"Choose an installation folder.",
                    L"Zanna Tools Setup",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    HostOptions staged = *context.options;
    staged.scope =
        SendDlgItemMessageW(context.window, kIdMachineScope, BM_GETCHECK, 0, 0) == BST_CHECKED
            ? InstallScope::Machine
            : InstallScope::User;
    staged.destination = destination;
    staged.selectedComponents.clear();
    for (const ComponentControl &component : context.components) {
        if (SendMessageW(component.checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED)
            staged.selectedComponents.insert(component.metadata->id);
    }
    staged.componentsSpecified = true;
    staged.componentPreset = ComponentPreset::Unspecified;
    staged.addToPath =
        SendDlgItemMessageW(context.window, kIdPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
    staged.registerAssociations =
        SendDlgItemMessageW(context.window, kIdAssociations, BM_GETCHECK, 0, 0) == BST_CHECKED;
    staged.createShortcuts =
        SendDlgItemMessageW(context.window, kIdShortcuts, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!DestroyWindow(context.window))
        throw std::runtime_error("cannot close the native setup options window");
    *context.options = std::move(staged);
    context.accepted = true;
}

void scrollOptionsWindow(CustomDialogContext &context, int bar, int requestedPosition) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    if (!GetScrollInfo(context.window, bar, &info))
        return;
    const int maximum = std::max(info.nMin, info.nMax - static_cast<int>(info.nPage) + 1);
    const int position = std::clamp(requestedPosition, info.nMin, maximum);
    if (position == info.nPos)
        return;
    const int delta = info.nPos - position;
    info.fMask = SIF_POS;
    info.nPos = position;
    SetScrollInfo(context.window, bar, &info, TRUE);
    ScrollWindowEx(context.window,
                   bar == SB_HORZ ? delta : 0,
                   bar == SB_VERT ? delta : 0,
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
}

void handleOptionsScroll(CustomDialogContext &context, int bar, WPARAM wParam) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    if (!GetScrollInfo(context.window, bar, &info))
        return;
    int position = info.nPos;
    switch (LOWORD(wParam)) {
        case SB_LINEUP:
            position -= scaled(24, GetDpiForWindow(context.window));
            break;
        case SB_LINEDOWN:
            position += scaled(24, GetDpiForWindow(context.window));
            break;
        case SB_PAGEUP:
            position -= static_cast<int>(info.nPage);
            break;
        case SB_PAGEDOWN:
            position += static_cast<int>(info.nPage);
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            position = info.nTrackPos;
            break;
        case SB_TOP:
            position = info.nMin;
            break;
        case SB_BOTTOM:
            position = info.nMax;
            break;
        default:
            return;
    }
    scrollOptionsWindow(context, bar, position);
}

void updateOptionsScrollbars(CustomDialogContext &context) {
    RECT client{};
    GetClientRect(context.window, &client);
    SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE};
    horizontal.nMin = 0;
    horizontal.nMax = std::max(0, context.virtualWidth - 1);
    horizontal.nPage = static_cast<UINT>(std::max(1L, client.right - client.left));
    SetScrollInfo(context.window, SB_HORZ, &horizontal, TRUE);
    SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE};
    vertical.nMin = 0;
    vertical.nMax = std::max(0, context.virtualHeight - 1);
    vertical.nPage = static_cast<UINT>(std::max(1L, client.bottom - client.top));
    SetScrollInfo(context.window, SB_VERT, &vertical, TRUE);
}

void revealFocusedControl(CustomDialogContext &context, HWND control) {
    if (!control || control == context.window)
        return;
    RECT controlRect{};
    RECT client{};
    if (!GetWindowRect(control, &controlRect) || !GetClientRect(context.window, &client))
        return;
    MapWindowPoints(HWND_DESKTOP, context.window, reinterpret_cast<POINT *>(&controlRect), 2);
    constexpr int kMargin = 12;
    if (controlRect.top < kMargin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_VERT, &info);
        scrollOptionsWindow(context, SB_VERT, info.nPos + controlRect.top - kMargin);
    } else if (controlRect.bottom > client.bottom - kMargin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_VERT, &info);
        scrollOptionsWindow(
            context, SB_VERT, info.nPos + controlRect.bottom - client.bottom + kMargin);
    }
    if (controlRect.left < kMargin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_HORZ, &info);
        scrollOptionsWindow(context, SB_HORZ, info.nPos + controlRect.left - kMargin);
    } else if (controlRect.right > client.right - kMargin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_HORZ, &info);
        scrollOptionsWindow(
            context, SB_HORZ, info.nPos + controlRect.right - client.right + kMargin);
    }
}

LRESULT CALLBACK customWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *context =
        reinterpret_cast<CustomDialogContext *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        context = static_cast<CustomDialogContext *>(create->lpCreateParams);
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context)) == 0 &&
            GetLastError() != ERROR_SUCCESS) {
            return FALSE;
        }
    }
    if (!context)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            if (!dc)
                return DefWindowProcW(window, message, wParam, lParam);
            RECT client{};
            if (GetClientRect(window, &client)) {
                drawInstallerBackdrop(dc, client, 0, *context->theme);
                const UINT dpi = context->theme->dpi();
                RECT mark{scaled(22, dpi), scaled(11, dpi), scaled(58, dpi), scaled(47, dpi)};
                drawInstallerBrandMark(dc, mark, *context->theme);
                const int saved = SaveDC(dc);
                if (saved != 0) {
                    SetBkMode(dc, TRANSPARENT);
                    SelectObject(dc, context->theme->monoBoldFont());
                    SetTextColor(dc, context->theme->textColor());
                    RECT name{scaled(70, dpi), scaled(8, dpi), scaled(222, dpi), scaled(29, dpi)};
                    DrawTextW(dc,
                              L"ZANNA",
                              -1,
                              &name,
                              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    SelectObject(dc, context->theme->monoFont());
                    SetTextColor(dc, context->theme->accentColor(InstallerAccent::Green));
                    RECT mode{scaled(70, dpi), scaled(27, dpi), scaled(222, dpi), scaled(49, dpi)};
                    DrawTextW(dc,
                              L"CUSTOM BUILD",
                              -1,
                              &mode,
                              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    RestoreDC(dc, saved);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(context->defaultButtonId, DC_HASDEFID);
        case DM_SETDEFID:
            context->defaultButtonId = LOWORD(wParam);
            return TRUE;
        case WM_DRAWITEM: {
            const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
            if (!item)
                break;
            InstallerAccent accent = InstallerAccent::Steel;
            switch (static_cast<int>(item->CtlID)) {
                case kIdAccept:
                case kIdTypical:
                    accent = InstallerAccent::Green;
                    break;
                case kIdBrowse:
                case kIdSDK:
                case kIdComplete:
                    accent = InstallerAccent::Teal;
                    break;
                default:
                    break;
            }
            drawInstallerActionButton(*item, accent, *context->theme);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return reinterpret_cast<LRESULT>(colorInstallerControl(message,
                                                                   reinterpret_cast<HDC>(wParam),
                                                                   reinterpret_cast<HWND>(lParam),
                                                                   *context->theme));
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == EN_SETFOCUS)
                revealFocusedControl(*context, reinterpret_cast<HWND>(lParam));
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            switch (LOWORD(wParam)) {
                case kIdBrowse:
                    try {
                        browseForDestination(*context);
                    } catch (const std::exception &) {
                        MessageBoxW(window,
                                    L"Setup could not open or update the installation folder.",
                                    L"Zanna Tools Setup",
                                    MB_OK | MB_ICONWARNING);
                    }
                    return 0;
                case kIdMinimal:
                    selectPreset(*context, ComponentPreset::Minimal);
                    return 0;
                case kIdTypical:
                    selectPreset(*context, ComponentPreset::Typical);
                    return 0;
                case kIdSDK:
                    selectPreset(*context, ComponentPreset::SDK);
                    return 0;
                case kIdComplete:
                    selectPreset(*context, ComponentPreset::Complete);
                    return 0;
                case kIdUserScope:
                case kIdMachineScope: {
                    const InstallScope scope =
                        LOWORD(wParam) == kIdUserScope ? InstallScope::User : InstallScope::Machine;
                    try {
                        const std::wstring destination =
                            defaultDestination(*context->package, scope).wstring();
                        if (!SetWindowTextW(context->destination, destination.c_str()))
                            throw std::runtime_error("cannot update the installation folder");
                        context->displayedScope = scope;
                        if (context->acceptButton)
                            SendMessageW(context->acceptButton,
                                         BCM_SETSHIELD,
                                         0,
                                         scope == InstallScope::Machine ? TRUE : FALSE);
                    } catch (...) {
                        CheckRadioButton(window,
                                         kIdUserScope,
                                         kIdMachineScope,
                                         context->displayedScope == InstallScope::User
                                             ? kIdUserScope
                                             : kIdMachineScope);
                        MessageBoxW(window,
                                    L"Setup could not select the default installation folder.",
                                    L"Zanna Tools Setup",
                                    MB_OK | MB_ICONWARNING);
                    }
                    return 0;
                }
                case kIdAccept:
                    try {
                        acceptCustomDialog(*context);
                    } catch (const std::exception &) {
                        MessageBoxW(window,
                                    L"Setup could not read the installation folder.",
                                    L"Zanna Tools Setup",
                                    MB_OK | MB_ICONWARNING);
                    }
                    return 0;
                case IDCANCEL:
                    DestroyWindow(window);
                    return 0;
                default:
                    if (LOWORD(wParam) >= kIdFirstComponent &&
                        LOWORD(wParam) <
                            kIdFirstComponent + static_cast<int>(context->components.size())) {
                        updateAssociationControl(*context);
                        return 0;
                    }
                    break;
            }
            break;
        case WM_SIZE:
            updateOptionsScrollbars(*context);
            return 0;
        case WM_DPICHANGED:
            updateOptionsDpi(*context, LOWORD(wParam), reinterpret_cast<const RECT *>(lParam));
            return 0;
        case WM_VSCROLL:
            handleOptionsScroll(*context, SB_VERT, wParam);
            return 0;
        case WM_HSCROLL:
            handleOptionsScroll(*context, SB_HORZ, wParam);
            return 0;
        case WM_MOUSEWHEEL: {
            SCROLLINFO info{sizeof(info), SIF_POS};
            GetScrollInfo(window, SB_VERT, &info);
            const int steps = consumeOptionsWheelSteps(wParam, context->wheelDelta);
            scrollOptionsWindow(
                *context, SB_VERT, info.nPos - steps * scaled(72, context->theme->dpi()));
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            context->window = nullptr;
            break;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM registerCustomWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = customWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance,
                                                      MAKEINTRESOURCEW(IDI_ZANNA_INSTALLER),
                                                      IMAGE_ICON,
                                                      0,
                                                      0,
                                                      LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"ZannaInstallerOptionsWindowV2";
    windowClass.hIconSm = windowClass.hIcon;
    const ATOM atom = registerVerifiedInstallerWindowClass(windowClass);
    if (!atom)
        throw std::runtime_error("cannot register the native setup window");
    return atom;
}

bool showCustomDialog(HINSTANCE instance,
                      const HostPackage &package,
                      const fs::path &initialDestination,
                      InstallScope initialScope,
                      const std::set<std::string> &initialComponents,
                      bool scopeLocked,
                      HostOptions &options) {
    if (!instance)
        throw std::runtime_error("native setup options require a module instance");
    registerCustomWindowClass(instance);
    CustomDialogContext context;
    context.instance = instance;
    context.package = &package;
    context.options = &options;
    context.initialDestination = initialDestination;
    context.initialScope = initialScope;
    context.initialComponents = initialComponents;
    context.scopeLocked = scopeLocked;
    context.displayedScope = initialScope;
    const UINT dpi = normalizeInstallerDpi(GetDpiForSystem());
    context.theme = std::make_unique<InstallerThemeResources>(dpi);

    struct DialogResources {
        CustomDialogContext &context;

        ~DialogResources() {
            if (context.window && IsWindow(context.window))
                DestroyWindow(context.window);
        }
    } resources{context};

    const int componentHeight = static_cast<int>(package.metadata.components.size()) * 31;
    const int clientHeight = 460 + componentHeight;
    context.virtualWidth = scaled(660, dpi);
    context.virtualHeight = scaled(clientHeight, dpi);
    RECT bounds{0, 0, scaled(680, dpi), scaled(clientHeight, dpi)};
    if (!AdjustWindowRectExForDpi(&bounds,
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX |
                                      WS_MAXIMIZEBOX | WS_VSCROLL | WS_HSCROLL,
                                  FALSE,
                                  WS_EX_DLGMODALFRAME,
                                  dpi)) {
        throw std::runtime_error("cannot calculate the native setup window size");
    }
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int requestedWidth = bounds.right - bounds.left;
    const int requestedHeight = bounds.bottom - bounds.top;
    const int maximumWidth =
        std::max(320, static_cast<int>(workArea.right - workArea.left) - scaled(24, dpi));
    const int maximumHeight =
        std::max(240, static_cast<int>(workArea.bottom - workArea.top) - scaled(24, dpi));
    const int windowWidth = std::min(requestedWidth, maximumWidth);
    const int windowHeight = std::min(requestedHeight, maximumHeight);
    context.window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                     L"ZannaInstallerOptionsWindowV2",
                                     L"Customize Zanna Tools Setup",
                                     WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX |
                                         WS_MAXIMIZEBOX | WS_VSCROLL | WS_HSCROLL,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     windowWidth,
                                     windowHeight,
                                     nullptr,
                                     nullptr,
                                     instance,
                                     &context);
    if (!context.window)
        throw std::runtime_error("cannot create the native setup options window");
    applyInstallerWindowTheme(context.window, *context.theme);
    setControlFont(context.window, context.theme->bodyFont());

    const std::wstring introduction = utf8ToWide(package.metadata.displayName) + L" " +
                                      utf8ToWide(package.metadata.version) +
                                      L" — choose the developer setup you want.";
    HWND introductionControl = createControl(
        context, 0, L"STATIC", introduction.c_str(), SS_LEFT, 240, 18, 407, 28, -1, dpi);
    setControlFont(introductionControl, context.theme->monoFont());
    createControl(
        context, 0, L"BUTTON", L"Installation scope", BS_GROUPBOX, 20, 50, 630, 74, -1, dpi);
    HWND userScope = createControl(context,
                                   0,
                                   L"BUTTON",
                                   L"Install for me (no administrator approval)",
                                   BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                                   38,
                                   74,
                                   285,
                                   24,
                                   kIdUserScope,
                                   dpi);
    HWND machineScope = createControl(context,
                                      0,
                                      L"BUTTON",
                                      L"Install for everyone",
                                      BS_AUTORADIOBUTTON | WS_TABSTOP,
                                      338,
                                      74,
                                      250,
                                      24,
                                      kIdMachineScope,
                                      dpi);
    SendMessageW(
        initialScope == InstallScope::User ? userScope : machineScope, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(userScope, scopeLocked ? FALSE : TRUE);
    EnableWindow(machineScope, scopeLocked ? FALSE : TRUE);
    createControl(
        context, 0, L"STATIC", L"Installation folder:", SS_LEFT, 24, 137, 145, 22, -1, dpi);
    context.destination = createControl(context,
                                        WS_EX_STATICEDGE,
                                        L"EDIT",
                                        initialDestination.c_str(),
                                        ES_AUTOHSCROLL | WS_TABSTOP,
                                        24,
                                        159,
                                        525,
                                        27,
                                        kIdDestination,
                                        dpi);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Browse...",
                  BS_OWNERDRAW | WS_TABSTOP,
                  558,
                  157,
                  92,
                  30,
                  kIdBrowse,
                  dpi);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Components",
                  BS_GROUPBOX,
                  20,
                  200,
                  630,
                  64 + componentHeight,
                  -1,
                  dpi);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Minimal",
                  BS_OWNERDRAW | WS_TABSTOP,
                  36,
                  222,
                  106,
                  28,
                  kIdMinimal,
                  dpi);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Typical",
                  BS_OWNERDRAW | WS_TABSTOP,
                  150,
                  222,
                  106,
                  28,
                  kIdTypical,
                  dpi);
    createControl(
        context, 0, L"BUTTON", L"SDK", BS_OWNERDRAW | WS_TABSTOP, 264, 222, 106, 28, kIdSDK, dpi);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Complete",
                  BS_OWNERDRAW | WS_TABSTOP,
                  378,
                  222,
                  106,
                  28,
                  kIdComplete,
                  dpi);

    int y = 258;
    for (size_t i = 0; i < package.metadata.components.size(); ++i, y += 31) {
        const auto &component = package.metadata.components[i];
        std::wstring label =
            utf8ToWide(component.label) + L"  (" + formatBytes(component.sizeBytes) + L")";
        if (component.required)
            label += L" - required";
        HWND checkbox = createControl(context,
                                      0,
                                      L"BUTTON",
                                      label.c_str(),
                                      BS_AUTOCHECKBOX | WS_TABSTOP,
                                      38,
                                      y,
                                      590,
                                      26,
                                      kIdFirstComponent + static_cast<int>(i),
                                      dpi);
        const bool selected =
            component.required || initialComponents.find(component.id) != initialComponents.end();
        SendMessageW(checkbox, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
        if (component.required)
            EnableWindow(checkbox, FALSE);
        context.components.push_back({&component, checkbox});
    }

    const int integrationY = 278 + componentHeight;
    HWND pathOption = createControl(context,
                                    0,
                                    L"BUTTON",
                                    L"Add the Zanna bin folder to PATH (owned and reversible)",
                                    BS_AUTOCHECKBOX | WS_TABSTOP,
                                    30,
                                    integrationY,
                                    610,
                                    25,
                                    kIdPath,
                                    dpi);
    HWND associationOption = createControl(context,
                                           0,
                                           L"BUTTON",
                                           L"Add safe Open With entries for Zanna source files",
                                           BS_AUTOCHECKBOX | WS_TABSTOP,
                                           30,
                                           integrationY + 29,
                                           610,
                                           25,
                                           kIdAssociations,
                                           dpi);
    HWND shortcutOption = createControl(context,
                                        0,
                                        L"BUTTON",
                                        L"Create Start menu shortcuts",
                                        BS_AUTOCHECKBOX | WS_TABSTOP,
                                        30,
                                        integrationY + 58,
                                        610,
                                        25,
                                        kIdShortcuts,
                                        dpi);
    const bool pathAvailable = !package.metadata.pathRelativePath.empty();
    const bool associationAvailable = !package.metadata.associations.empty();
    const bool shortcutAvailable = !package.metadata.shortcuts.empty();
    const bool pathChecked =
        pathAvailable && options.addToPath.value_or(package.metadata.addToPath);
    const bool associationChecked =
        associationAvailable &&
        options.registerAssociations.value_or(package.metadata.registerFileAssociations);
    const bool shortcutChecked =
        shortcutAvailable && options.createShortcuts.value_or(package.metadata.createShortcuts);
    SendMessageW(pathOption, BM_SETCHECK, pathChecked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(
        associationOption, BM_SETCHECK, associationChecked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(shortcutOption, BM_SETCHECK, shortcutChecked ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(pathOption, pathAvailable);
    EnableWindow(associationOption, associationAvailable);
    EnableWindow(shortcutOption, shortcutAvailable);

    const int bottom = integrationY + 95;
    std::wstring note = L"Setup is self-contained and will not download dependencies. "
                        L"Component choices can be changed later from Installed apps.";
    createControl(context, 0, L"STATIC", note.c_str(), SS_LEFT, 24, bottom, 620, 42, -1, dpi);
    HWND accept = createControl(context,
                                0,
                                L"BUTTON",
                                L"Continue",
                                BS_OWNERDRAW | WS_TABSTOP,
                                438,
                                bottom + 52,
                                100,
                                32,
                                kIdAccept,
                                dpi);
    context.associationOption = associationOption;
    context.acceptButton = accept;
    updateAssociationControl(context);
    createControl(context,
                  0,
                  L"BUTTON",
                  L"Cancel",
                  BS_OWNERDRAW | WS_TABSTOP,
                  548,
                  bottom + 52,
                  100,
                  32,
                  IDCANCEL,
                  dpi);
    SendMessageW(accept, BCM_SETSHIELD, 0, initialScope == InstallScope::Machine ? TRUE : FALSE);
    updateOptionsScrollbars(context);

    RECT windowRect{};
    if (!GetWindowRect(context.window, &windowRect) ||
        !SetWindowPos(
            context.window,
            nullptr,
            workArea.left +
                ((workArea.right - workArea.left) - (windowRect.right - windowRect.left)) / 2,
            workArea.top +
                ((workArea.bottom - workArea.top) - (windowRect.bottom - windowRect.top)) / 2,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
        throw std::runtime_error("cannot position the native setup options window");
    }
    SetFocus(context.destination);

    MSG message{};
    while (context.window && IsWindow(context.window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
            throw std::runtime_error("cannot read the native setup message queue");
        if (result == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(context.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return context.accepted;
}

std::wstring selectedComponentSummary(const HostPackage &package,
                                      const HostOptions &options,
                                      const std::set<std::string> &initialComponents) {
    std::set<std::string> selected = initialComponents;
    if (options.componentsSpecified)
        selected = options.selectedComponents;
    std::wstring result;
    uint64_t bytes = 0;
    for (const auto &component : package.metadata.components) {
        bool enabled = component.required || selected.find(component.id) != selected.end();
        if (options.componentPreset == ComponentPreset::Minimal)
            enabled = component.required;
        else if (options.componentPreset == ComponentPreset::Typical)
            enabled = component.required || component.defaultSelected;
        else if (options.componentPreset == ComponentPreset::SDK)
            enabled = component.required || component.id == "sdk";
        else if (options.componentPreset == ComponentPreset::Complete)
            enabled = true;
        if (!enabled)
            continue;
        if (!result.empty())
            result += L", ";
        result += utf8ToWide(component.label);
        if (component.sizeBytes > std::numeric_limits<uint64_t>::max() - bytes)
            throw std::runtime_error("selected component size exceeds the supported range");
        bytes += component.sizeBytes;
    }
    result += L" (" + formatBytes(bytes) + L")";
    return result;
}

} // namespace

bool configureInstallerWizard(HINSTANCE instance,
                              const HostPackage &package,
                              const fs::path &initialDestination,
                              InstallScope initialScope,
                              const std::set<std::string> &initialComponents,
                              bool installationPresent,
                              HostOptions &options) {
    options.scope = initialScope;
    options.destination = initialDestination;
    for (;;) {
        const std::wstring title = utf8ToWide(package.metadata.displayName) + L" Setup";
        if (package.metadata.packageMode == "maintenance") {
            BrandedInstallerPage maintenance;
            maintenance.windowTitle = title;
            maintenance.eyebrow = L"// TOOLCHAIN CONTROL";
            maintenance.heading = L"Keep the stack sharp.";
            maintenance.body =
                L"Change the installed developer surface, restore it from the signed package, "
                L"or remove only the files and registrations Zanna owns.";
            maintenance.metadata = L"VERSION " + utf8ToWide(package.metadata.version) + L"  |  " +
                                   utf8ToWide(package.metadata.architecture) + L"  |  " +
                                   utf8ToWide(package.metadata.channel);
            maintenance.actions = {
                {kMaintenanceModify,
                 L"MODIFY THE TOOLCHAIN",
                 L"Choose components and developer integrations.",
                 InstallerAccent::Green},
                {kMaintenanceRepair,
                 L"REPAIR ZANNA",
                 L"Verify and atomically restore the selected installation.",
                 InstallerAccent::Steel},
                {kMaintenanceRemove,
                 L"UNINSTALL ZANNA",
                 L"Remove Zanna-owned files and registrations; preserve everything else.",
                 InstallerAccent::Danger}};
            if (!package.metadata.updateManifestUrl.empty())
                maintenance.actions.push_back({kMaintenanceUpdate,
                                               L"CHECK FOR UPDATES",
                                               L"Query the pinned, signed Zanna update service.",
                                               InstallerAccent::Teal});
            maintenance.defaultAction = kMaintenanceModify;
            maintenance.detailsLabel = L"SYSTEM PROFILE";
            maintenance.detailsText = dependencySummary();
            const int selected = showBrandedInstallerPage(instance, maintenance).action;
            if (selected == IDCANCEL)
                return false;
            if (selected == kMaintenanceUpdate) {
                checkUpdatesInteractive(instance, package);
                continue;
            } else if (selected == kMaintenanceModify) {
                options.operation = Operation::Modify;
                if (!showCustomDialog(instance,
                                      package,
                                      initialDestination,
                                      initialScope,
                                      initialComponents,
                                      true,
                                      options)) {
                    continue;
                }
            } else if (selected == kMaintenanceRepair) {
                options.operation = Operation::Repair;
            } else {
                options.operation = Operation::Uninstall;
            }
        } else {
            const std::wstring verb = installationPresent ? L"UPDATE" : L"INSTALL";
            BrandedInstallerPage welcome;
            welcome.windowTitle = title;
            welcome.eyebrow = L"// ZANNA TOOLCHAIN · WINDOWS";
            welcome.heading = L"Source in. Native code out.";
            welcome.body =
                L"A complete game-development toolchain, built from scratch. Choose a build "
                L"profile and setup will wire the compiler, Studio, SDK, PATH, file actions, "
                L"shortcuts, docs, and samples for you.";
            welcome.metadata = L"VERSION " + utf8ToWide(package.metadata.version) + L"  |  " +
                               utf8ToWide(package.metadata.architecture) + L"  |  " +
                               utf8ToWide(package.metadata.channel);
            welcome.actions = {{kWelcomeRecommended,
                                verb + L" RECOMMENDED",
                                L"Core tools, Studio, SDK, and available developer integrations.",
                                InstallerAccent::Green},
                               {kWelcomeSDK,
                                verb + L" SDK TOOLS",
                                L"Command-line toolchain and native development files.",
                                InstallerAccent::Steel},
                               {kWelcomeComplete,
                                verb + L" EVERYTHING",
                                L"Every packaged component, documentation set, and sample.",
                                InstallerAccent::Teal},
                               {kWelcomeCustom,
                                L"CUSTOM BUILD",
                                L"Choose scope, destination, components, and integrations.",
                                InstallerAccent::Teal}};
            if (!package.metadata.updateManifestUrl.empty())
                welcome.actions.push_back({kWelcomeUpdate,
                                           L"CHECK FOR UPDATES",
                                           L"Query the pinned, signed Zanna update service.",
                                           InstallerAccent::Steel});
            welcome.defaultAction = kWelcomeRecommended;
            welcome.detailsLabel = L"DEVELOPER READINESS";
            welcome.detailsText = dependencySummary();
            const int selected = showBrandedInstallerPage(instance, welcome).action;
            if (selected == IDCANCEL)
                return false;
            if (selected == kWelcomeUpdate) {
                checkUpdatesInteractive(instance, package);
                continue;
            }
            options.operation = Operation::Install;
            options.componentsSpecified = false;
            if (selected == kWelcomeRecommended) {
                options.componentPreset = ComponentPreset::Typical;
            } else if (selected == kWelcomeSDK) {
                options.componentPreset = ComponentPreset::SDK;
            } else if (selected == kWelcomeComplete) {
                options.componentPreset = ComponentPreset::Complete;
            } else if (!showCustomDialog(instance,
                                         package,
                                         initialDestination,
                                         initialScope,
                                         initialComponents,
                                         installationPresent,
                                         options)) {
                continue;
            }
        }

        BrandedInstallerPage ready;
        ready.windowTitle = title;
        ready.eyebrow = L"// TRANSACTION READY";
        if (options.operation == Operation::Uninstall) {
            ready.heading = L"Ready to remove Zanna.";
            ready.body =
                L"Setup will remove Zanna-owned files, shortcuts, PATH registration, and Open "
                L"With entries. Files you added to the installation remain untouched.";
            ready.metadata = L"RECOVERABLE · OWNERSHIP-AWARE · NO UNOWNED FILE DELETION";
            ready.actions.push_back({kReadyContinue,
                                     L"UNINSTALL ZANNA",
                                     L"Begin the reversible removal transaction.",
                                     InstallerAccent::Danger});
        } else if (options.operation == Operation::Repair) {
            ready.heading = L"Ready to restore the stack.";
            ready.body = L"Setup will verify installed hashes and atomically restore the selected "
                         L"toolchain from this signed, self-contained package.";
            ready.metadata = L"VERIFY · STAGE · COMMIT · ROLLBACK-SAFE";
            ready.actions.push_back({kReadyContinue,
                                     L"REPAIR ZANNA",
                                     L"Verify and restore the installed developer surface.",
                                     InstallerAccent::Green});
        } else {
            ready.heading = installationPresent ? L"Ready to evolve the toolchain."
                                                : L"Ready to bring Zanna online.";
            ready.body = L"Location: " + options.destination.wstring() + L"\r\nScope: " +
                         std::wstring(*options.scope == InstallScope::User ? L"Current user"
                                                                           : L"All users") +
                         L"\r\nComponents: " +
                         selectedComponentSummary(package, options, initialComponents);
            ready.metadata = L"SOURCE → IL → NATIVE · ATOMIC INSTALL · ZERO DOWNLOADS";
            ready.actions.push_back(
                {kReadyContinue,
                 installationPresent ? L"UPDATE ZANNA" : L"INSTALL ZANNA",
                 L"Stage, verify, and commit the complete developer setup.",
                 InstallerAccent::Green,
                 options.operation == Operation::Install && !package.licenseText.empty()});
        }
        ready.actions.push_back({kReadyBack,
                                 L"GO BACK",
                                 L"Return to the setup choices without changing the machine.",
                                 InstallerAccent::Steel});
        ready.defaultAction = kReadyContinue;
        if (options.operation == Operation::Install && !package.licenseText.empty()) {
            ready.detailsLabel = L"LICENSE TERMS";
            ready.detailsText = utf8ToWide(package.licenseText);
            ready.verificationText = L"I have read and accept the Zanna license terms.";
        }
        const int selected = showBrandedInstallerPage(instance, ready).action;
        if (selected == kReadyContinue)
            return true;
        if (selected == IDCANCEL)
            return false;
    }
}

int runInstallerProgress(HINSTANCE instance,
                         const HostPackage &package,
                         Operation operation,
                         UiLevel uiLevel,
                         Logger &logger,
                         const std::function<int()> &work) {
    if (uiLevel == UiLevel::Quiet)
        return work();
    std::wstring eyebrow;
    std::wstring action;
    std::wstring body;
    switch (operation) {
        case Operation::Uninstall:
            eyebrow = L"// OWNERSHIP-AWARE REMOVAL";
            action = L"Taking Zanna offline.";
            body = L"Removing owned files and registrations while preserving everything you "
                   L"added. If interrupted, setup restores the previous installation.";
            break;
        case Operation::Repair:
            eyebrow = L"// VERIFY · STAGE · RESTORE";
            action = L"Restoring the toolchain.";
            body = L"Checking every installed hash and atomically restoring the developer "
                   L"surface from this self-contained package.";
            break;
        case Operation::Modify:
            eyebrow = L"// RECONFIGURE THE STACK";
            action = L"Shaping your Zanna build.";
            body = L"Applying the selected components and integrations as one recoverable "
                   L"transaction.";
            break;
        default:
            eyebrow = L"// SOURCE → IL → NATIVE";
            action = L"Bringing Zanna online.";
            body = L"Staging, verifying, and committing the compiler, Studio, SDK, developer "
                   L"integrations, documentation, and samples.";
            break;
    }
    const std::wstring title = utf8ToWide(package.metadata.displayName) + L" Setup";
    return runBrandedInstallerProgress(instance, title, eyebrow, action, body, logger, work);
}

void showInstallerFinish(HINSTANCE instance,
                         const HostPackage &package,
                         const fs::path &installRoot,
                         const std::set<std::string> &,
                         HostOptions &options) {
    std::vector<BrandedInstallerAction> actions;
    actions.reserve(7U);
    const std::string launchRelative = package.metadata.productKind == "toolchain" &&
                                               !package.metadata.associationExecutable.empty()
                                           ? package.metadata.associationExecutable
                                           : package.metadata.executableName;
    const fs::path primary = installRoot / utf8ToWide(launchRelative);
    std::error_code fileError;
    if (fs::is_regular_file(primary, fileError) && !fileError) {
        actions.push_back({kFinishLaunch,
                           package.metadata.productKind == "toolchain"
                               ? L"LAUNCH ZANNA STUDIO"
                               : L"LAUNCH " + utf8ToWide(package.metadata.displayName),
                           L"Start creating with the newly installed toolchain.",
                           InstallerAccent::Green});
    }
    if (package.metadata.productKind == "toolchain") {
        actions.push_back({kFinishPrompt,
                           L"OPEN DEVELOPER PROMPT",
                           L"Start a terminal with the Zanna environment ready.",
                           InstallerAccent::Steel});
        actions.push_back({kFinishQuickstart,
                           L"READ THE QUICK START",
                           L"Open the Windows developer setup guide.",
                           InstallerAccent::Teal});
        fileError.clear();
        if (fs::is_directory(installRoot / L"share" / L"zanna" / L"samples", fileError) &&
            !fileError) {
            actions.push_back({kFinishSamples,
                               L"EXPLORE THE SAMPLES",
                               L"Open the installed source and game examples.",
                               InstallerAccent::Teal});
        }
        actions.push_back({kFinishCopyVerification,
                           L"COPY VERIFICATION COMMAND",
                           L"Verify this installation from any new terminal.",
                           InstallerAccent::Steel});
    }
    actions.push_back({kFinishClose,
                       L"FINISH",
                       L"Close setup. Your developer environment is ready.",
                       InstallerAccent::Green});
    std::wstring content = L"The installation completed successfully. PATH and Open With "
                           L"changes are available to new applications.";
    for (;;) {
        BrandedInstallerPage finish;
        finish.windowTitle = utf8ToWide(package.metadata.displayName) + L" Setup";
        finish.eyebrow = L"// TOOLCHAIN ONLINE";
        finish.heading = L"Build something impossible.";
        finish.body = content;
        finish.metadata = L"VERSION " + utf8ToWide(package.metadata.version) + L"  |  " +
                          utf8ToWide(package.metadata.architecture) +
                          L"  |  READY FOR NEW TERMINALS";
        finish.actions = actions;
        finish.defaultAction = actions.empty() ? kFinishClose : actions.front().id;
        finish.closeAction = kFinishClose;
        finish.showCancel = false;
        const int selected = showBrandedInstallerPage(instance, finish).action;
        if (selected == kFinishCopyVerification) {
            const fs::path zanna = installRoot / L"bin" / L"zanna.exe";
            try {
                copyTextToClipboard(quoteCommandLineArgument(zanna.wstring()) + L" --version");
                content = L"Verification command copied to the clipboard. Paste it into a new "
                          L"terminal to confirm the installation.";
            } catch (const std::exception &error) {
                content = L"Windows could not copy the verification command: " +
                          utf8ToWide(error.what()) + L"\r\n\r\nRun this command manually:\r\n" +
                          quoteCommandLineArgument(zanna.wstring()) + L" --version";
            }
            continue;
        }
        options.launchIDE = selected == kFinishLaunch;
        options.launchPrompt = selected == kFinishPrompt;
        options.openQuickstart = selected == kFinishQuickstart;
        options.openSamples = selected == kFinishSamples;
        return;
    }
}

} // namespace zanna::installer
