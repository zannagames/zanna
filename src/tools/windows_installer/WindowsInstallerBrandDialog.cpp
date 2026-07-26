//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements the branded native choice and cooperative-progress windows.
///
/// Native controls retain keyboard focus, accessible names, and dialog
/// navigation. Modal contexts own theme resources and outlive their HWNDs;
/// worker progress is copied through bounded, coalesced UI messages. Every
/// Win32 callback prevents exceptions from crossing the native boundary.
///
/// @see WindowsInstallerBrandDialog.hpp
/// @see WindowsInstallerTheme.cpp
/// @see WindowsInstallerWizard.cpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerBrandDialog.hpp"
#include "WindowsInstallerBrandValidation.hpp"
#include "WindowsInstallerResources.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zanna::installer {
namespace {

constexpr wchar_t kPageClassName[] = L"ZannaInstallerBrandPageV1";
constexpr wchar_t kProgressClassName[] = L"ZannaInstallerBrandProgressV1";

constexpr int kIdEyebrow = 3101;
constexpr int kIdHeading = 3102;
constexpr int kIdBody = 3103;
constexpr int kIdMetadata = 3104;
constexpr int kIdDetailsLabel = 3105;
constexpr int kIdDetails = 3106;
constexpr int kIdVerification = 3107;
constexpr int kIdStatus = 3108;
constexpr int kIdCancel = IDCANCEL;

constexpr UINT kMessageProgressText = WM_APP + 41U;
constexpr UINT kMessageProgressComplete = WM_APP + 42U;
constexpr UINT_PTR kProgressTimer = 1U;
constexpr size_t kMaximumProgressStatusUnits = 2048U;

/// @brief Scale a logical 96-DPI coordinate to a target DPI.
/// @param value Logical pixel value.
/// @param dpi Normalized target dots per inch.
/// @return Device-pixel coordinate rounded by MulDiv.
int scaled(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

/// @brief Apply a font to an existing native control.
/// @param control Control handle, ignored when null.
/// @param font Font handle, ignored when null.
void setControlFont(HWND control, HFONT font) noexcept {
    if (control && font)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

/// @brief Convert lone line feeds to CRLF for a multiline Win32 edit control.
/// @param text Source display text.
/// @return Owned text preserving existing CRLF pairs.
std::wstring windowsEditText(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    wchar_t previous = L'\0';
    for (const wchar_t character : text) {
        if (character == L'\n' && previous != L'\r')
            result.push_back(L'\r');
        result.push_back(character);
        previous = character;
    }
    return result;
}

/// @brief Resolve and validate the desktop work area available to setup.
/// @return Usable screen work-area rectangle, falling back to screen metrics.
/// @throws std::runtime_error When the resulting dimensions are invalid.
RECT installerWorkArea() {
    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int64_t width = static_cast<int64_t>(workArea.right) - workArea.left;
    const int64_t height = static_cast<int64_t>(workArea.bottom) - workArea.top;
    if (width <= 0 || width > std::numeric_limits<int>::max() || height <= 0 ||
        height > std::numeric_limits<int>::max()) {
        throw std::runtime_error("cannot resolve a usable installer work area");
    }
    return workArea;
}

/// @brief Center and reveal a window within the installer work area.
/// @param window Window to inspect and position.
/// @param workArea Bounding desktop work area.
/// @throws std::runtime_error When bounds cannot be queried or positioning fails.
void centerAndShow(HWND window, const RECT &workArea) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds))
        throw std::runtime_error("cannot inspect the branded installer window");
    if (!SetWindowPos(
            window,
            nullptr,
            workArea.left + ((workArea.right - workArea.left) - (bounds.right - bounds.left)) / 2,
            workArea.top + ((workArea.bottom - workArea.top) - (bounds.bottom - bounds.top)) / 2,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW)) {
        throw std::runtime_error("cannot position the branded installer window");
    }
}

/// @brief Accumulate high-resolution wheel deltas into integral scroll steps.
/// @param value Mouse-wheel message parameter.
/// @param remainder Receives the unconsumed delta remainder.
/// @return Number of complete WHEEL_DELTA steps, possibly negative.
int consumeWheelSteps(WPARAM value, int &remainder) noexcept {
    remainder += GET_WHEEL_DELTA_WPARAM(value);
    const int steps = remainder / WHEEL_DELTA;
    remainder -= steps * WHEEL_DELTA;
    return steps;
}

/// @brief Apply the operating system's suggested bounds after a DPI change.
/// @param window Window to reposition.
/// @param bounds Suggested rectangle, ignored when null or degenerate.
void applySuggestedDpiBounds(HWND window, const RECT *bounds) noexcept {
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

struct PageActionControl {
    const BrandedInstallerAction *action{nullptr};
    HWND window{nullptr};
};

struct PageContext {
    HINSTANCE instance;
    const BrandedInstallerPage &page;
    InstallerThemeResources theme;
    std::vector<PageActionControl> actions;
    HWND window{nullptr};
    HWND verification{nullptr};
    HWND status{nullptr};
    HWND initialFocus{nullptr};
    BrandedInstallerPageResult result;
    int virtualWidth{0};
    int virtualHeight{0};
    int brandPanelWidth{0};
    int wheelDelta{0};
    int defaultButtonId{0};
    bool compact{false};

    /// @brief Construct modal page state and initialize its default result.
    /// @param pageInstance Module instance used to create controls.
    /// @param pageModel Borrowed page model valid for the modal lifetime.
    /// @param dpi Initial normalized window DPI.
    PageContext(HINSTANCE pageInstance, const BrandedInstallerPage &pageModel, UINT dpi)
        : instance(pageInstance), page(pageModel), theme(dpi) {
        result.action = page.closeAction;
        defaultButtonId = page.defaultAction;
    }
};

void updatePageScrollbars(PageContext &context) noexcept;

/// @brief Rebuild page theme resources and rescale children for a DPI change.
/// @param context Live page context.
/// @param requestedDpi DPI reported by WM_DPICHANGED.
/// @param suggestedBounds Optional operating-system window rectangle.
/// @details Allocation or theming failures are suppressed at the Win32 boundary.
void updatePageDpi(PageContext &context, UINT requestedDpi, const RECT *suggestedBounds) noexcept {
    const UINT newDpi = normalizeInstallerDpi(requestedDpi);
    const UINT oldDpi = context.theme.dpi();
    try {
        InstallerThemeResources replacement(newDpi);
        if (oldDpi != newDpi) {
            (void)rescaleInstallerChildWindows(context.window, oldDpi, newDpi);
            context.virtualWidth =
                MulDiv(context.virtualWidth, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.virtualHeight =
                MulDiv(context.virtualHeight, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.brandPanelWidth =
                MulDiv(context.brandPanelWidth, static_cast<int>(newDpi), static_cast<int>(oldDpi));
        }
        context.theme = std::move(replacement);
        applyInstallerWindowTheme(context.window, context.theme);
        setControlFont(GetDlgItem(context.window, kIdEyebrow), context.theme.monoBoldFont());
        setControlFont(GetDlgItem(context.window, kIdHeading), context.theme.headingFont());
        setControlFont(GetDlgItem(context.window, kIdBody), context.theme.bodyFont());
        setControlFont(GetDlgItem(context.window, kIdMetadata), context.theme.monoFont());
        setControlFont(GetDlgItem(context.window, kIdDetailsLabel), context.theme.monoBoldFont());
        setControlFont(GetDlgItem(context.window, kIdDetails), context.theme.monoFont());
        setControlFont(context.verification, context.theme.bodyFont());
        setControlFont(context.status, context.theme.bodyBoldFont());
        for (const PageActionControl &action : context.actions)
            setControlFont(action.window, context.theme.bodyBoldFont());
        setControlFont(GetDlgItem(context.window, kIdCancel), context.theme.bodyBoldFont());
        for (HWND child = GetWindow(context.window, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            applyInstallerControlTheme(child, context.theme);
        }
    } catch (...) {
        return;
    }
    applySuggestedDpiBounds(context.window, suggestedBounds);
    updatePageScrollbars(context);
    InvalidateRect(context.window, nullptr, TRUE);
}

/// @brief Create, font, and theme one page child control.
/// @param context Live page context providing parent, instance, theme, and DPI.
/// @param exStyle Extended window styles.
/// @param className Native window class name.
/// @param text Initial accessible/display text.
/// @param style Child-specific window styles.
/// @param x Logical left coordinate.
/// @param y Logical top coordinate.
/// @param width Logical width.
/// @param height Logical height.
/// @param id Dialog control identifier.
/// @param font Font applied to the control.
/// @return Created child HWND.
/// @throws std::runtime_error When CreateWindowExW fails.
HWND createPageControl(PageContext &context,
                       DWORD exStyle,
                       const wchar_t *className,
                       const wchar_t *text,
                       DWORD style,
                       int x,
                       int y,
                       int width,
                       int height,
                       int id,
                       HFONT font) {
    HWND control = CreateWindowExW(exStyle,
                                   className,
                                   text,
                                   style | WS_CHILD | WS_VISIBLE,
                                   scaled(x, context.theme.dpi()),
                                   scaled(y, context.theme.dpi()),
                                   scaled(width, context.theme.dpi()),
                                   scaled(height, context.theme.dpi()),
                                   context.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   context.instance,
                                   nullptr);
    if (!control)
        throw std::runtime_error("cannot create a branded setup control");
    setControlFont(control, font);
    applyInstallerControlTheme(control, context.theme);
    return control;
}

/// @brief Move the page viewport to a bounded scrollbar position.
/// @param context Live page context.
/// @param bar @c SB_HORZ or @c SB_VERT.
/// @param requestedPosition Desired logical scroll position.
void scrollPage(PageContext &context, int bar, int requestedPosition) noexcept {
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

/// @brief Interpret a Win32 scrollbar command for the page viewport.
/// @param context Live page context.
/// @param bar @c SB_HORZ or @c SB_VERT.
/// @param value Scroll-message parameter containing command and thumb data.
void handlePageScroll(PageContext &context, int bar, WPARAM value) noexcept {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    if (!GetScrollInfo(context.window, bar, &info))
        return;
    int position = info.nPos;
    switch (LOWORD(value)) {
        case SB_LINEUP:
            position -= scaled(32, context.theme.dpi());
            break;
        case SB_LINEDOWN:
            position += scaled(32, context.theme.dpi());
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
    scrollPage(context, bar, position);
}

/// @brief Synchronize page scrollbar ranges and page sizes with the client area.
/// @param context Live page context containing virtual content dimensions.
void updatePageScrollbars(PageContext &context) noexcept {
    RECT client{};
    if (!GetClientRect(context.window, &client))
        return;
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

/// @brief Scroll the page until a focused child is visible with a margin.
/// @param context Live page context.
/// @param control Child control to reveal.
void revealPageControl(PageContext &context, HWND control) noexcept {
    if (!control || control == context.window)
        return;
    RECT controlBounds{};
    RECT client{};
    if (!GetWindowRect(control, &controlBounds) || !GetClientRect(context.window, &client))
        return;
    MapWindowPoints(HWND_DESKTOP, context.window, reinterpret_cast<POINT *>(&controlBounds), 2);
    const int margin = scaled(18, context.theme.dpi());
    if (controlBounds.top < margin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_VERT, &info);
        scrollPage(context, SB_VERT, info.nPos + controlBounds.top - margin);
    } else if (controlBounds.bottom > client.bottom - margin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_VERT, &info);
        scrollPage(context, SB_VERT, info.nPos + controlBounds.bottom - client.bottom + margin);
    }
    if (controlBounds.left < margin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_HORZ, &info);
        scrollPage(context, SB_HORZ, info.nPos + controlBounds.left - margin);
    } else if (controlBounds.right > client.right - margin) {
        SCROLLINFO info{sizeof(info), SIF_POS};
        GetScrollInfo(context.window, SB_HORZ, &info);
        scrollPage(context, SB_HORZ, info.nPos + controlBounds.right - client.right + margin);
    }
}

/// @brief Draw the compact-layout brand mark and labels.
/// @param dc Paint device context.
/// @param client Current client rectangle.
/// @param context Page theme and layout state.
void paintCompactHeader(HDC dc, const RECT &client, const PageContext &context) noexcept {
    if (!context.compact)
        return;
    const int left = scaled(24, context.theme.dpi());
    const int top = scaled(22, context.theme.dpi());
    const int size = scaled(52, context.theme.dpi());
    RECT mark{left, top, left + size, top + size};
    drawInstallerBrandMark(dc, mark, context.theme);
    RECT name{mark.right + scaled(16, context.theme.dpi()),
              top,
              client.right - scaled(20, context.theme.dpi()),
              top + scaled(28, context.theme.dpi())};
    const int saved = SaveDC(dc);
    if (saved == 0)
        return;
    SelectObject(dc, context.theme.monoBoldFont());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, context.theme.textColor());
    DrawTextW(dc, L"ZANNA", -1, &name, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RECT platform{
        name.left, name.bottom, name.right, name.bottom + scaled(26, context.theme.dpi())};
    SelectObject(dc, context.theme.monoFont());
    SetTextColor(dc, context.theme.accentColor(InstallerAccent::Green));
    DrawTextW(dc,
              L"DEVELOPER PLATFORM",
              -1,
              &platform,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    RestoreDC(dc, saved);
}

/// @brief Validate verification state and commit a selected page action.
/// @param context Live page context whose result is updated.
/// @param action Action requested by keyboard or mouse activation.
/// @details A required unchecked verification control receives focus and leaves
///          the modal page open.
void acceptPageAction(PageContext &context, const BrandedInstallerAction &action) noexcept {
    if (action.requiresVerification && !context.page.verificationText.empty() &&
        context.verification &&
        SendMessageW(context.verification, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        SetWindowTextW(context.status, L"Review and accept the license terms to continue.");
        ShowWindow(context.status, SW_SHOW);
        SetFocus(context.verification);
        return;
    }
    context.result.action = action.id;
    context.result.verificationChecked =
        context.verification &&
        SendMessageW(context.verification, BM_GETCHECK, 0, 0) == BST_CHECKED;
    DestroyWindow(context.window);
}

/// @brief Dispatch native messages for the branded choice page.
/// @param window Page HWND.
/// @param message Win32 message identifier.
/// @param wParam Message-specific word parameter.
/// @param lParam Message-specific long parameter.
/// @return Message result, delegating unhandled messages to DefWindowProcW.
LRESULT CALLBACK pageWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *context = reinterpret_cast<PageContext *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        context = static_cast<PageContext *>(create->lpCreateParams);
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
                drawInstallerBackdrop(dc, client, context->brandPanelWidth, context->theme);
                paintCompactHeader(dc, client, *context);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case DM_GETDEFID:
            return context->defaultButtonId > 0 ? MAKELRESULT(context->defaultButtonId, DC_HASDEFID)
                                                : 0;
        case DM_SETDEFID:
            context->defaultButtonId = LOWORD(wParam);
            return TRUE;
        case WM_DRAWITEM: {
            const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
            if (!item)
                break;
            InstallerAccent accent = InstallerAccent::Steel;
            const auto action =
                std::find_if(context->actions.begin(),
                             context->actions.end(),
                             /// @brief Match a rendered owner-draw control to its page action.
                             /// @param candidate Candidate action-control binding.
                             /// @return `true` when its action identifier equals the control ID.
                             [&](const PageActionControl &candidate) {
                                 return candidate.action &&
                                        candidate.action->id == static_cast<int>(item->CtlID);
                             });
            if (action != context->actions.end())
                accent = action->action->accent;
            else if (item->CtlID == static_cast<UINT>(kIdCancel))
                accent = InstallerAccent::Steel;
            drawInstallerActionButton(*item, accent, context->theme);
            return TRUE;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            const int id = control ? GetDlgCtrlID(control) : 0;
            HBRUSH brush = colorInstallerControl(message, dc, control, context->theme);
            if (id == kIdEyebrow)
                SetTextColor(dc, context->theme.accentColor(InstallerAccent::Green));
            else if (id == kIdBody || id == kIdMetadata || id == kIdDetailsLabel)
                SetTextColor(dc, context->theme.textDimColor());
            else if (id == kIdStatus)
                SetTextColor(dc, context->theme.accentColor(InstallerAccent::Warning));
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == EN_SETFOCUS)
                revealPageControl(*context, reinterpret_cast<HWND>(lParam));
            if (HIWORD(wParam) == BN_CLICKED) {
                const auto action =
                    std::find_if(context->actions.begin(),
                                 context->actions.end(),
                                 /// @brief Match a clicked control identifier to its page action.
                                 /// @param candidate Candidate action-control binding.
                                 /// @return `true` when its action identifier equals `id`.
                                 [&](const PageActionControl &candidate) {
                                     return candidate.action && candidate.action->id == id;
                                 });
                if (action != context->actions.end()) {
                    acceptPageAction(*context, *action->action);
                    return 0;
                }
                if (id == kIdCancel) {
                    context->result.action = context->page.closeAction;
                    DestroyWindow(window);
                    return 0;
                }
            }
            break;
        }
        case WM_SIZE:
            updatePageScrollbars(*context);
            return 0;
        case WM_DPICHANGED:
            updatePageDpi(*context, LOWORD(wParam), reinterpret_cast<const RECT *>(lParam));
            return 0;
        case WM_VSCROLL:
            handlePageScroll(*context, SB_VERT, wParam);
            return 0;
        case WM_HSCROLL:
            handlePageScroll(*context, SB_HORZ, wParam);
            return 0;
        case WM_MOUSEWHEEL: {
            SCROLLINFO info{sizeof(info), SIF_POS};
            GetScrollInfo(window, SB_VERT, &info);
            const int steps = consumeWheelSteps(wParam, context->wheelDelta);
            scrollPage(*context, SB_VERT, info.nPos - steps * scaled(72, context->theme.dpi()));
            return 0;
        }
        case WM_CLOSE:
            context->result.action = context->page.closeAction;
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            context->window = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/// @brief Register or verify the branded page window class.
/// @param instance Module instance owning the class and icon resource.
/// @return Registered class atom.
/// @throws std::runtime_error When registration or verification fails.
ATOM registerPageWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = pageWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance,
                                                      MAKEINTRESOURCEW(IDI_ZANNA_INSTALLER),
                                                      IMAGE_ICON,
                                                      0,
                                                      0,
                                                      LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kPageClassName;
    windowClass.hIconSm = windowClass.hIcon;
    const ATOM atom = registerVerifiedInstallerWindowClass(windowClass);
    if (!atom)
        throw std::runtime_error("cannot register the branded setup page");
    return atom;
}

struct ProgressContext {
    HINSTANCE instance;
    InstallerThemeResources theme;
    std::wstring windowTitle;
    std::wstring eyebrow;
    std::wstring heading;
    std::wstring body;
    Logger &logger;
    std::function<int()> work;
    HWND window{nullptr};
    HWND status{nullptr};
    HWND cancel{nullptr};
    std::thread worker;
    std::exception_ptr failure;
    std::atomic<bool> cancellationRequested{false};
    std::atomic<bool> completed{false};
    std::mutex statusMutex;
    std::wstring pendingStatus;
    bool statusMessagePosted{false};
    int result{kExitFatalError};
    int brandPanelWidth{0};
    bool compact{false};
    unsigned animationPhase{0};
    int trackTop{0};
    int virtualWidth{0};
    int virtualHeight{0};
    int wheelDelta{0};

    /// @brief Construct progress-window state from borrowed work and logger callbacks.
    /// @param progressInstance Module instance used to create controls.
    /// @param dpi Initial normalized window DPI.
    /// @param titleText Window caption.
    /// @param eyebrowText Short brand/context label.
    /// @param headingText Primary operation heading.
    /// @param bodyText Explanatory operation text.
    /// @param progressLogger Borrowed logger valid until the modal call returns.
    /// @param progressWork Borrowed callable copied into owned context storage.
    ProgressContext(HINSTANCE progressInstance,
                    UINT dpi,
                    std::wstring_view titleText,
                    std::wstring_view eyebrowText,
                    std::wstring_view headingText,
                    std::wstring_view bodyText,
                    Logger &progressLogger,
                    const std::function<int()> &progressWork)
        : instance(progressInstance), theme(dpi), windowTitle(titleText), eyebrow(eyebrowText),
          heading(headingText), body(bodyText), logger(progressLogger), work(progressWork) {}
};

/// @brief Coalesce a bounded worker status update onto the UI message queue.
/// @param context Live progress context shared with the worker.
/// @param message Status text, truncated to the configured UTF-16-unit limit.
/// @details Allocation, mutex, and posting failures are suppressed because this
///          function can run from arbitrary lifecycle callbacks.
void postProgressStatus(ProgressContext &context, std::wstring_view message) noexcept {
    bool shouldPost = false;
    try {
        std::wstring bounded(message.substr(0, kMaximumProgressStatusUnits));
        std::lock_guard lock(context.statusMutex);
        context.pendingStatus = std::move(bounded);
        if (!context.statusMessagePosted) {
            context.statusMessagePosted = true;
            shouldPost = true;
        }
    } catch (...) {
        return;
    }
    if (shouldPost && !PostMessageW(context.window, kMessageProgressText, 0, 0)) {
        std::lock_guard lock(context.statusMutex);
        context.statusMessagePosted = false;
    }
}

/// @brief Rebuild progress theme resources and rescale controls for a DPI change.
/// @param context Live progress context.
/// @param requestedDpi DPI reported by WM_DPICHANGED.
/// @param suggestedBounds Optional operating-system window rectangle.
void updateProgressDpi(ProgressContext &context,
                       UINT requestedDpi,
                       const RECT *suggestedBounds) noexcept {
    const UINT newDpi = normalizeInstallerDpi(requestedDpi);
    const UINT oldDpi = context.theme.dpi();
    try {
        InstallerThemeResources replacement(newDpi);
        if (oldDpi != newDpi) {
            (void)rescaleInstallerChildWindows(context.window, oldDpi, newDpi);
            context.virtualWidth =
                MulDiv(context.virtualWidth, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.virtualHeight =
                MulDiv(context.virtualHeight, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.brandPanelWidth =
                MulDiv(context.brandPanelWidth, static_cast<int>(newDpi), static_cast<int>(oldDpi));
            context.trackTop =
                MulDiv(context.trackTop, static_cast<int>(newDpi), static_cast<int>(oldDpi));
        }
        context.theme = std::move(replacement);
        applyInstallerWindowTheme(context.window, context.theme);
        setControlFont(GetDlgItem(context.window, kIdEyebrow), context.theme.monoBoldFont());
        setControlFont(GetDlgItem(context.window, kIdHeading), context.theme.headingFont());
        setControlFont(GetDlgItem(context.window, kIdBody), context.theme.bodyFont());
        setControlFont(context.status, context.theme.bodyFont());
        setControlFont(context.cancel, context.theme.bodyBoldFont());
        for (HWND child = GetWindow(context.window, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            applyInstallerControlTheme(child, context.theme);
        }
    } catch (...) {
        return;
    }
    applySuggestedDpiBounds(context.window, suggestedBounds);
    InvalidateRect(context.window, nullptr, TRUE);
}

/// @brief Create, font, and theme one progress-window child control.
/// @param context Live progress context providing parent, theme, and DPI.
/// @param className Native window class name.
/// @param text Initial accessible/display text.
/// @param style Child-specific styles.
/// @param x Logical left coordinate.
/// @param y Logical top coordinate.
/// @param width Logical width.
/// @param height Logical height.
/// @param id Dialog control identifier.
/// @param font Font applied to the control.
/// @return Created child HWND.
/// @throws std::runtime_error When CreateWindowExW fails.
HWND createProgressControl(ProgressContext &context,
                           const wchar_t *className,
                           const wchar_t *text,
                           DWORD style,
                           int x,
                           int y,
                           int width,
                           int height,
                           int id,
                           HFONT font) {
    HWND control = CreateWindowExW(0,
                                   className,
                                   text,
                                   style | WS_CHILD | WS_VISIBLE,
                                   scaled(x, context.theme.dpi()),
                                   scaled(y, context.theme.dpi()),
                                   scaled(width, context.theme.dpi()),
                                   scaled(height, context.theme.dpi()),
                                   context.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   context.instance,
                                   nullptr);
    if (!control)
        throw std::runtime_error("cannot create the branded progress control");
    setControlFont(control, font);
    applyInstallerControlTheme(control, context.theme);
    return control;
}

/// @brief Draw the indeterminate animated progress track.
/// @param dc Paint device context.
/// @param client Current client rectangle.
/// @param context Animation, scrolling, layout, and theme state.
void paintProgressTrack(HDC dc, const RECT &client, const ProgressContext &context) noexcept {
    SCROLLINFO horizontal{sizeof(horizontal), SIF_POS};
    SCROLLINFO vertical{sizeof(vertical), SIF_POS};
    GetScrollInfo(context.window, SB_HORZ, &horizontal);
    GetScrollInfo(context.window, SB_VERT, &vertical);
    const int left = (context.compact ? scaled(28, context.theme.dpi())
                                      : context.brandPanelWidth + scaled(38, context.theme.dpi())) -
                     horizontal.nPos;
    const int right =
        std::min(static_cast<int>(client.right) - scaled(38, context.theme.dpi()),
                 context.virtualWidth - scaled(38, context.theme.dpi()) - horizontal.nPos);
    const int top = context.trackTop - vertical.nPos;
    const int height = std::max(4, scaled(6, context.theme.dpi()));
    if (right <= left)
        return;
    RECT track{left, top, right, top + height};
    HBRUSH trackBrush = CreateSolidBrush(context.theme.borderColor());
    if (trackBrush) {
        FillRect(dc, &track, trackBrush);
        DeleteObject(trackBrush);
    }
    const int width = right - left;
    const int pulseWidth = std::max(scaled(64, context.theme.dpi()), width / 5);
    const int travel = width + pulseWidth;
    const int pulseLeft = left +
                          static_cast<int>((static_cast<uint64_t>(context.animationPhase) * 7U) %
                                           static_cast<unsigned>(std::max(1, travel))) -
                          pulseWidth;
    RECT pulse{
        std::max(left, pulseLeft), top, std::min(right, pulseLeft + pulseWidth), top + height};
    if (pulse.right > pulse.left) {
        const InstallerAccent accent = (context.animationPhase / 36U) % 2U == 0U
                                           ? InstallerAccent::Green
                                           : InstallerAccent::Teal;
        HBRUSH pulseBrush = CreateSolidBrush(context.theme.accentColor(accent));
        if (pulseBrush) {
            FillRect(dc, &pulse, pulseBrush);
            DeleteObject(pulseBrush);
        }
    }
}

/// @brief Record cooperative cancellation and disable repeated cancellation.
/// @param context Live progress context shared with the worker.
/// @details Completed work ignores cancellation; active work receives an atomic
///          signal through the logger callback.
void requestProgressCancellation(ProgressContext &context) noexcept {
    if (context.completed.load())
        return;
    context.cancellationRequested.store(true);
    if (context.cancel)
        EnableWindow(context.cancel, FALSE);
    if (context.status) {
        SetWindowTextW(context.status,
                       L"Cancelling safely and restoring the previous installation...");
    }
}

/// @brief Dispatch native messages for the cooperative progress window.
/// @param window Progress HWND.
/// @param message Win32 message identifier.
/// @param wParam Message-specific word parameter.
/// @param lParam Message-specific long parameter.
/// @return Message result, delegating unhandled messages to DefWindowProcW.
LRESULT CALLBACK progressWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *context = reinterpret_cast<ProgressContext *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        context = static_cast<ProgressContext *>(create->lpCreateParams);
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
                drawInstallerBackdrop(dc, client, context->brandPanelWidth, context->theme);
                if (context->compact) {
                    const int left = scaled(24, context->theme.dpi());
                    const int top = scaled(22, context->theme.dpi());
                    const int size = scaled(52, context->theme.dpi());
                    RECT mark{left, top, left + size, top + size};
                    drawInstallerBrandMark(dc, mark, context->theme);
                }
                paintProgressTrack(dc, client, *context);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kProgressTimer) {
                ++context->animationPhase;
                RECT client{};
                GetClientRect(window, &client);
                InvalidateRect(window, &client, FALSE);
                if (context->completed.load())
                    PostMessageW(window, kMessageProgressComplete, 0, 0);
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
            if (item) {
                drawInstallerActionButton(*item, InstallerAccent::Steel, context->theme);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            HBRUSH brush = colorInstallerControl(message, dc, control, context->theme);
            const int id = control ? GetDlgCtrlID(control) : 0;
            if (id == kIdEyebrow)
                SetTextColor(dc, context->theme.accentColor(InstallerAccent::Green));
            else if (id == kIdBody || id == kIdStatus)
                SetTextColor(dc, context->theme.textDimColor());
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == kIdCancel && HIWORD(wParam) == BN_CLICKED) {
                requestProgressCancellation(*context);
                return 0;
            }
            break;
        case WM_SIZE: {
            RECT client{};
            if (GetClientRect(window, &client)) {
                SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE};
                horizontal.nMin = 0;
                horizontal.nMax = std::max(0, context->virtualWidth - 1);
                horizontal.nPage = static_cast<UINT>(std::max(1L, client.right - client.left));
                SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
                SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE};
                vertical.nMin = 0;
                vertical.nMax = std::max(0, context->virtualHeight - 1);
                vertical.nPage = static_cast<UINT>(std::max(1L, client.bottom - client.top));
                SetScrollInfo(window, SB_VERT, &vertical, TRUE);
            }
            return 0;
        }
        case WM_DPICHANGED:
            updateProgressDpi(*context, LOWORD(wParam), reinterpret_cast<const RECT *>(lParam));
            return 0;
        case WM_VSCROLL:
        case WM_HSCROLL: {
            const int bar = message == WM_VSCROLL ? SB_VERT : SB_HORZ;
            SCROLLINFO info{sizeof(info), SIF_ALL};
            if (!GetScrollInfo(window, bar, &info))
                return 0;
            int position = info.nPos;
            switch (LOWORD(wParam)) {
                case SB_LINEUP:
                    position -= scaled(32, context->theme.dpi());
                    break;
                case SB_LINEDOWN:
                    position += scaled(32, context->theme.dpi());
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
                default:
                    return 0;
            }
            const int maximum = std::max(info.nMin, info.nMax - static_cast<int>(info.nPage) + 1);
            position = std::clamp(position, info.nMin, maximum);
            if (position != info.nPos) {
                const int delta = info.nPos - position;
                info.fMask = SIF_POS;
                info.nPos = position;
                SetScrollInfo(window, bar, &info, TRUE);
                ScrollWindowEx(window,
                               bar == SB_HORZ ? delta : 0,
                               bar == SB_VERT ? delta : 0,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            SCROLLINFO info{sizeof(info), SIF_ALL};
            if (!GetScrollInfo(window, SB_VERT, &info))
                return 0;
            const int steps = consumeWheelSteps(wParam, context->wheelDelta);
            const int maximum = std::max(info.nMin, info.nMax - static_cast<int>(info.nPage) + 1);
            const int position = std::clamp(
                info.nPos - steps * scaled(72, context->theme.dpi()), info.nMin, maximum);
            if (position != info.nPos) {
                const int delta = info.nPos - position;
                info.fMask = SIF_POS;
                info.nPos = position;
                SetScrollInfo(window, SB_VERT, &info, TRUE);
                ScrollWindowEx(window,
                               0,
                               delta,
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr,
                               SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN);
            }
            return 0;
        }
        case kMessageProgressText: {
            std::wstring text;
            {
                std::lock_guard lock(context->statusMutex);
                text.swap(context->pendingStatus);
                context->statusMessagePosted = false;
            }
            if (!text.empty() && context->status)
                SetWindowTextW(context->status, text.c_str());
            return 0;
        }
        case kMessageProgressComplete:
            if (context->completed.load()) {
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (context->completed.load())
                DestroyWindow(window);
            else
                requestProgressCancellation(*context);
            return 0;
        case WM_NCDESTROY:
            KillTimer(window, kProgressTimer);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            context->window = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/// @brief Register or verify the branded progress window class.
/// @param instance Module instance owning the class and icon resource.
/// @return Registered class atom.
/// @throws std::runtime_error When registration or verification fails.
ATOM registerProgressWindowClass(HINSTANCE instance) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = progressWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance,
                                                      MAKEINTRESOURCEW(IDI_ZANNA_INSTALLER),
                                                      IMAGE_ICON,
                                                      0,
                                                      0,
                                                      LR_DEFAULTSIZE | LR_SHARED));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kProgressClassName;
    windowClass.hIconSm = windowClass.hIcon;
    const ATOM atom = registerVerifiedInstallerWindowClass(windowClass);
    if (!atom)
        throw std::runtime_error("cannot register the branded setup progress window");
    return atom;
}

} // namespace

/// @brief Display one validated branded action page modally.
/// @param instance Module instance used for classes, icons, and controls.
/// @param page Borrowed content/action model valid for the modal call.
/// @return Selected action and final verification-checkbox state.
/// @throws std::runtime_error On invalid model data, native creation failures,
///         or message-queue errors.
BrandedInstallerPageResult showBrandedInstallerPage(HINSTANCE instance,
                                                    const BrandedInstallerPage &page) {
    validateBrandedInstallerPage(instance, page);
    registerPageWindowClass(instance);

    const UINT dpi = normalizeInstallerDpi(GetDpiForSystem());
    PageContext context(instance, page, dpi);
    const RECT workArea = installerWorkArea();
    const int maximumWidth =
        std::max(320, static_cast<int>(workArea.right - workArea.left) - scaled(24, dpi));
    const int maximumHeight =
        std::max(240, static_cast<int>(workArea.bottom - workArea.top) - scaled(24, dpi));
    context.compact = scaled(920, dpi) > maximumWidth;
    const int logicalWidth = context.compact ? 650 : 920;
    const int contentX = context.compact ? 28 : 320;
    const int contentWidth = context.compact ? 592 : 570;
    const int contentTop = context.compact ? 108 : 52;
    context.brandPanelWidth = context.compact ? 0 : scaled(282, dpi);

    int y = contentTop;
    const int actionHeight = 58;
    const int actionGap = 10;
    const int detailsHeight = page.detailsText.empty() ? 0 : 116;
    const int actionsHeight =
        static_cast<int>(page.actions.size()) * (actionHeight + actionGap) - actionGap;
    const int logicalHeight = y + 34 + 58 + 62 + 28 + actionsHeight +
                              (detailsHeight == 0 ? 0 : detailsHeight + 42) +
                              (page.verificationText.empty() ? 0 : 38) + 88;
    context.virtualWidth = scaled(logicalWidth - 24, dpi);
    context.virtualHeight = scaled(std::max(620, logicalHeight), dpi);

    RECT bounds{
        0, 0, scaled(logicalWidth, dpi), scaled(std::min(690, std::max(620, logicalHeight)), dpi)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX |
                        WS_MAXIMIZEBOX | WS_VSCROLL | WS_HSCROLL;
    if (!AdjustWindowRectExForDpi(
            &bounds, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, dpi)) {
        throw std::runtime_error("cannot calculate the branded setup page size");
    }
    const int adjustedWidth = static_cast<int>(bounds.right - bounds.left);
    const int adjustedHeight = static_cast<int>(bounds.bottom - bounds.top);
    const int windowWidth = std::min(adjustedWidth, maximumWidth);
    const int windowHeight = std::min(adjustedHeight, maximumHeight);
    context.window = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_APPWINDOW,
                                     kPageClassName,
                                     page.windowTitle.c_str(),
                                     style,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     windowWidth,
                                     windowHeight,
                                     nullptr,
                                     nullptr,
                                     instance,
                                     &context);
    if (!context.window)
        throw std::runtime_error("cannot create the branded setup page");

    struct WindowGuard {
        PageContext &context;

        ~WindowGuard() {
            if (context.window && IsWindow(context.window))
                DestroyWindow(context.window);
        }
    } guard{context};

    applyInstallerWindowTheme(context.window, context.theme);
    createPageControl(context,
                      0,
                      L"STATIC",
                      page.eyebrow.c_str(),
                      SS_LEFT,
                      contentX,
                      y,
                      contentWidth,
                      26,
                      kIdEyebrow,
                      context.theme.monoBoldFont());
    y += 34;
    createPageControl(context,
                      0,
                      L"STATIC",
                      page.heading.c_str(),
                      SS_LEFT,
                      contentX,
                      y,
                      contentWidth,
                      54,
                      kIdHeading,
                      context.theme.headingFont());
    y += 58;
    createPageControl(context,
                      0,
                      L"STATIC",
                      page.body.c_str(),
                      SS_LEFT,
                      contentX,
                      y,
                      contentWidth,
                      56,
                      kIdBody,
                      context.theme.bodyFont());
    y += 62;
    createPageControl(context,
                      0,
                      L"STATIC",
                      page.metadata.c_str(),
                      SS_LEFT,
                      contentX,
                      y,
                      contentWidth,
                      22,
                      kIdMetadata,
                      context.theme.monoFont());
    y += 34;

    for (size_t index = 0; index < page.actions.size(); ++index) {
        const BrandedInstallerAction &action = page.actions[index];
        std::wstring label = action.title;
        if (!action.description.empty())
            label += L"\n" + action.description;
        HWND actionWindow = createPageControl(context,
                                              0,
                                              L"BUTTON",
                                              label.c_str(),
                                              BS_OWNERDRAW | WS_TABSTOP,
                                              contentX,
                                              y,
                                              contentWidth,
                                              actionHeight,
                                              action.id,
                                              context.theme.bodyBoldFont());
        context.actions.push_back({&action, actionWindow});
        if (!context.initialFocus || action.id == page.defaultAction)
            context.initialFocus = actionWindow;
        y += actionHeight + actionGap;
    }

    if (!page.detailsText.empty()) {
        y += 4;
        createPageControl(context,
                          0,
                          L"STATIC",
                          page.detailsLabel.c_str(),
                          SS_LEFT,
                          contentX,
                          y,
                          contentWidth,
                          22,
                          kIdDetailsLabel,
                          context.theme.monoBoldFont());
        y += 26;
        const std::wstring detailsText = windowsEditText(page.detailsText);
        HWND details =
            createPageControl(context,
                              WS_EX_STATICEDGE,
                              L"EDIT",
                              detailsText.c_str(),
                              ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                              contentX,
                              y,
                              contentWidth,
                              detailsHeight,
                              kIdDetails,
                              context.theme.monoFont());
        SendMessageW(details, EM_SETREADONLY, TRUE, 0);
        y += detailsHeight + 10;
    }

    if (!page.verificationText.empty()) {
        context.verification = createPageControl(context,
                                                 0,
                                                 L"BUTTON",
                                                 page.verificationText.c_str(),
                                                 BS_AUTOCHECKBOX | WS_TABSTOP,
                                                 contentX,
                                                 y,
                                                 contentWidth,
                                                 30,
                                                 kIdVerification,
                                                 context.theme.bodyFont());
        y += 36;
    }
    context.status = createPageControl(context,
                                       0,
                                       L"STATIC",
                                       L"",
                                       SS_LEFT,
                                       contentX,
                                       y,
                                       contentWidth - 122,
                                       38,
                                       kIdStatus,
                                       context.theme.bodyBoldFont());
    ShowWindow(context.status, SW_HIDE);
    if (page.showCancel) {
        createPageControl(context,
                          0,
                          L"BUTTON",
                          page.cancelText.c_str(),
                          BS_OWNERDRAW | WS_TABSTOP,
                          contentX + contentWidth - 112,
                          y,
                          112,
                          38,
                          kIdCancel,
                          context.theme.bodyBoldFont());
    }
    context.virtualHeight = std::max(context.virtualHeight, scaled(y + 64, dpi));
    updatePageScrollbars(context);
    centerAndShow(context.window, workArea);
    if (context.initialFocus)
        SetFocus(context.initialFocus);

    MSG message{};
    while (context.window && IsWindow(context.window)) {
        const BOOL messageResult = GetMessageW(&message, nullptr, 0, 0);
        if (messageResult == -1)
            throw std::runtime_error("cannot read the branded setup message queue");
        if (messageResult == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(context.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return context.result;
}

/// @brief Run lifecycle work on a worker behind a modal progress surface.
/// @param instance Module instance used for native resources.
/// @param windowTitle Window caption.
/// @param eyebrow Short brand/context label.
/// @param heading Primary operation heading.
/// @param body Explanatory operation text.
/// @param logger Logger temporarily configured with progress and cancellation callbacks.
/// @param work Lifecycle callable executed exactly once on the worker thread.
/// @return Work callable's exit code.
/// @throws std::runtime_error On validation/native UI failures; rethrows any
///         exception captured from @p work after joining the worker.
int runBrandedInstallerProgress(HINSTANCE instance,
                                std::wstring_view windowTitle,
                                std::wstring_view eyebrow,
                                std::wstring_view heading,
                                std::wstring_view body,
                                Logger &logger,
                                const std::function<int()> &work) {
    validateBrandedInstallerProgress(instance, windowTitle, eyebrow, heading, body, work);
    registerProgressWindowClass(instance);
    const UINT dpi = normalizeInstallerDpi(GetDpiForSystem());
    ProgressContext context(instance, dpi, windowTitle, eyebrow, heading, body, logger, work);
    const RECT workArea = installerWorkArea();
    const int maximumWidth =
        std::max(320, static_cast<int>(workArea.right - workArea.left) - scaled(24, dpi));
    const int maximumHeight =
        std::max(240, static_cast<int>(workArea.bottom - workArea.top) - scaled(24, dpi));
    context.compact = scaled(820, dpi) > maximumWidth;
    const int logicalWidth = context.compact ? 620 : 820;
    constexpr int kLogicalHeight = 390;
    const int contentX = context.compact ? 28 : 284;
    const int contentWidth = context.compact ? 564 : 500;
    const int contentTop = context.compact ? 108 : 66;
    context.brandPanelWidth = context.compact ? 0 : scaled(248, dpi);
    context.virtualWidth = scaled(logicalWidth - 24, dpi);
    context.virtualHeight = scaled(kLogicalHeight, dpi);

    RECT bounds{0, 0, scaled(logicalWidth, dpi), scaled(kLogicalHeight, dpi)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX |
                        WS_MAXIMIZEBOX | WS_VSCROLL | WS_HSCROLL;
    if (!AdjustWindowRectExForDpi(
            &bounds, style, FALSE, WS_EX_CONTROLPARENT | WS_EX_APPWINDOW, dpi)) {
        throw std::runtime_error("cannot calculate the branded progress window size");
    }
    context.window =
        CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_APPWINDOW,
                        kProgressClassName,
                        context.windowTitle.c_str(),
                        style,
                        CW_USEDEFAULT,
                        CW_USEDEFAULT,
                        std::min(static_cast<int>(bounds.right - bounds.left), maximumWidth),
                        std::min(static_cast<int>(bounds.bottom - bounds.top), maximumHeight),
                        nullptr,
                        nullptr,
                        instance,
                        &context);
    if (!context.window)
        throw std::runtime_error("cannot create the branded progress window");

    struct ProgressGuard {
        ProgressContext &context;

        ~ProgressGuard() {
            context.cancellationRequested.store(true);
            if (context.worker.joinable())
                context.worker.join();
            context.logger.setProgressCallback({});
            context.logger.setCancellationCallback({});
            if (context.window && IsWindow(context.window))
                DestroyWindow(context.window);
        }
    } guard{context};

    applyInstallerWindowTheme(context.window, context.theme);
    int y = contentTop;
    createProgressControl(context,
                          L"STATIC",
                          context.eyebrow.c_str(),
                          SS_LEFT,
                          contentX,
                          y,
                          contentWidth,
                          26,
                          kIdEyebrow,
                          context.theme.monoBoldFont());
    y += 36;
    createProgressControl(context,
                          L"STATIC",
                          context.heading.c_str(),
                          SS_LEFT,
                          contentX,
                          y,
                          contentWidth,
                          58,
                          kIdHeading,
                          context.theme.headingFont());
    y += 66;
    createProgressControl(context,
                          L"STATIC",
                          context.body.c_str(),
                          SS_LEFT,
                          contentX,
                          y,
                          contentWidth,
                          52,
                          kIdBody,
                          context.theme.bodyFont());
    y += 82;
    context.trackTop = scaled(y - 20, context.theme.dpi());
    context.status = createProgressControl(context,
                                           L"STATIC",
                                           L"Verifying package and preparing the transaction...",
                                           SS_LEFT,
                                           contentX,
                                           y,
                                           contentWidth,
                                           48,
                                           kIdStatus,
                                           context.theme.bodyFont());
    context.cancel = createProgressControl(context,
                                           L"BUTTON",
                                           L"Cancel safely",
                                           BS_OWNERDRAW | WS_TABSTOP,
                                           contentX + contentWidth - 132,
                                           y + 58,
                                           132,
                                           38,
                                           kIdCancel,
                                           context.theme.bodyBoldFont());
    centerAndShow(context.window, workArea);
    if (SetTimer(context.window, kProgressTimer, 35U, nullptr) == 0)
        throw std::runtime_error("cannot start the branded progress animation timer");
    SetFocus(context.cancel);

    /// @brief Forward one lifecycle progress message to the branded progress window.
    /// @param message Status text to publish asynchronously.
    logger.setProgressCallback(
        [&context](std::wstring_view message) noexcept { postProgressStatus(context, message); });
    /// @brief Report whether the user has requested cooperative cancellation.
    /// @return Current cancellation-request flag.
    logger.setCancellationCallback([&context] { return context.cancellationRequested.load(); });
    const HWND progressWindow = context.window;
    /// @brief Execute the lifecycle operation and notify the progress window on completion.
    /// @details Captures any exception for propagation by the UI thread.
    context.worker = std::thread([progressWindow, &context] {
        try {
            context.result = context.work();
        } catch (...) {
            context.failure = std::current_exception();
        }
        context.completed.store(true);
        PostMessageW(progressWindow, kMessageProgressComplete, 0, 0);
    });

    MSG message{};
    while (context.window && IsWindow(context.window)) {
        const BOOL messageResult = GetMessageW(&message, nullptr, 0, 0);
        if (messageResult == -1)
            throw std::runtime_error("cannot read the branded progress message queue");
        if (messageResult == 0) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(context.window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (context.worker.joinable())
        context.worker.join();
    logger.setProgressCallback({});
    logger.setCancellationCallback({});
    if (context.failure)
        std::rethrow_exception(context.failure);
    return context.result;
}

} // namespace zanna::installer
