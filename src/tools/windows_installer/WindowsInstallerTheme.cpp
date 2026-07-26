//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerTheme.cpp
/// @brief Implements the native dark Zanna Games installer visual system.
///
/// Brand values remain RGB rather than @c COLORREF literals, while high-contrast rendering uses
/// only Windows system colors and retains focus outlines. Painting restores selected GDI objects
/// before deletion, and noexcept drawing helpers cannot unwind through Win32 callbacks.
///
/// InstallerThemeResources owns long-lived fonts and brushes. Painting functions release locally
/// created pens and temporary brushes before returning.
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerTheme.hpp"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace zanna::installer {
namespace {

constexpr InstallerBrandPalette kPalette{
    0x0C1214,
    0x10181B,
    0x131C20,
    0x1A2E30,
    0x0F191C,
    0x24343A,
    0x33474F,
    0xDDE7E4,
    0x9AB0AB,
    0x6F827F,
    0x8CC63F,
    0x6DA230,
    0xA9B8BD,
    0x2BC8C4,
    0x189AA8,
    0xE8B04C,
    0xFF6B6B,
};

/// @brief Convert canonical @c 0xRRGGBB brand data to Windows BGR storage.
/// @param rgb Canonical RGB value.
/// @return Equivalent GDI @c COLORREF.
COLORREF toColorRef(uint32_t rgb) noexcept {
    return RGB((rgb >> 16U) & 0xFFU, (rgb >> 8U) & 0xFFU, rgb & 0xFFU);
}

/// @brief Scale a 96-DPI pixel metric to the active DPI.
/// @param value Baseline pixel metric.
/// @param dpi Normalized destination DPI.
/// @return Integer metric produced by Win32 proportional scaling.
int scaled(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

/// @brief Allocate one ClearType installer font.
/// @param dpi Normalized destination DPI.
/// @param points Logical point size.
/// @param weight Win32 font weight.
/// @param face Preferred installed family name.
/// @param pitch Pitch and family flags.
/// @return Owned GDI font handle.
/// @throws std::runtime_error If Windows cannot allocate the font.
HFONT createFont(UINT dpi, int points, int weight, const wchar_t *face, BYTE pitch) {
    HFONT font = CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72),
                             0,
                             0,
                             0,
                             weight,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY,
                             pitch,
                             face);
    if (!font)
        throw std::runtime_error("cannot create the Zanna installer interface font");
    return font;
}

/// @brief Mark a font-family enumeration as having found at least one match.
/// @param data Pointer-sized reference to the caller's Boolean result.
/// @return Zero to stop enumeration after the first match.
int CALLBACK noteFontFamily(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM data) {
    *reinterpret_cast<bool *>(data) = true;
    return 0;
}

/// @brief Determine whether a named font family is available to the desktop.
/// @param face NUL-terminated family name.
/// @return @c true when Windows enumerates at least one matching family.
bool fontFamilyAvailable(const wchar_t *face) noexcept {
    HDC dc = GetDC(nullptr);
    if (!dc)
        return false;
    LOGFONTW candidate{};
    candidate.lfCharSet = DEFAULT_CHARSET;
    if (wcsncpy_s(candidate.lfFaceName, LF_FACESIZE, face, _TRUNCATE) != 0) {
        ReleaseDC(nullptr, dc);
        return false;
    }
    bool found = false;
    EnumFontFamiliesExW(dc, &candidate, noteFontFamily, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

/// @brief Convert an eight-bit sRGB channel to linear-light intensity.
/// @param value Channel value from zero through 255.
/// @return Linear intensity from zero through one.
double linearChannel(uint32_t value) noexcept {
    const double channel = static_cast<double>(value) / 255.0;
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

/// @brief Calculate WCAG relative luminance for a canonical RGB value.
/// @param rgb Color encoded as @c 0xRRGGBB.
/// @return Relative luminance from zero through one.
double relativeLuminance(uint32_t rgb) noexcept {
    return 0.2126 * linearChannel((rgb >> 16U) & 0xFFU) +
           0.7152 * linearChannel((rgb >> 8U) & 0xFFU) + 0.0722 * linearChannel(rgb & 0xFFU);
}

/// @brief Delete and clear an owned font handle.
/// @param font Handle slot to release and set to null.
void deleteFont(HFONT &font) noexcept {
    if (font)
        DeleteObject(font);
    font = nullptr;
}

/// @brief Delete and clear an owned brush handle.
/// @param brush Handle slot to release and set to null.
void deleteBrush(HBRUSH &brush) noexcept {
    if (brush)
        DeleteObject(brush);
    brush = nullptr;
}

/// @brief Draw decorative circuit traces, nodes, and rings behind the brand mark.
/// @param dc Destination device context.
/// @param bounds Brand-panel rectangle.
/// @param theme Active colors, DPI, and accessibility mode.
void drawCircuitField(HDC dc, const RECT &bounds, const InstallerThemeResources &theme) noexcept {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0)
        return;

    const std::array<InstallerAccent, 3> accents = {
        InstallerAccent::Green, InstallerAccent::Steel, InstallerAccent::Teal};
    const int saved = SaveDC(dc);
    if (saved == 0)
        return;
    SetBkMode(dc, TRANSPARENT);
    for (size_t index = 0; index < accents.size(); ++index) {
        const COLORREF color =
            theme.highContrast() ? GetSysColor(COLOR_GRAYTEXT) : theme.accentColor(accents[index]);
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        if (!pen)
            continue;
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int inset = scaled(20 + static_cast<int>(index) * 27, theme.dpi());
        const int y = bounds.top + height / 4 + static_cast<int>(index) * height / 5;
        MoveToEx(dc, bounds.left + inset, y, nullptr);
        LineTo(dc, bounds.left + width / 2, y);
        LineTo(dc, bounds.left + width / 2 + scaled(22, theme.dpi()), y - scaled(22, theme.dpi()));
        LineTo(dc, bounds.right - inset, y - scaled(22, theme.dpi()));
        const int node = scaled(3, theme.dpi());
        Ellipse(dc, bounds.left + inset - node, y - node, bounds.left + inset + node, y + node);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    HPEN ringPen =
        CreatePen(PS_SOLID,
                  1,
                  theme.highContrast() ? GetSysColor(COLOR_GRAYTEXT) : theme.borderBrightColor());
    if (ringPen) {
        HGDIOBJ oldPen = SelectObject(dc, ringPen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        const int size = std::min(width, height) - scaled(48, theme.dpi());
        const int left = bounds.left + (width - size) / 2;
        const int top = bounds.top + scaled(40, theme.dpi());
        Ellipse(dc, left, top, left + size, top + size);
        const int inner = scaled(14, theme.dpi());
        Ellipse(dc, left + inner, top + inner, left + size - inner, top + size - inner);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(ringPen);
    }
    RestoreDC(dc, saved);
}

/// @brief Draw a single transparent-background text line with borrowed resources.
/// @param dc Destination device context.
/// @param bounds Text layout rectangle.
/// @param text NUL-terminated text.
/// @param font Borrowed font to select temporarily.
/// @param color Text color.
/// @param format Additional @c DrawTextW formatting flags.
void drawTextLine(HDC dc,
                  const RECT &bounds,
                  const wchar_t *text,
                  HFONT font,
                  COLORREF color,
                  UINT format) noexcept {
    const int saved = SaveDC(dc);
    if (saved == 0)
        return;
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT textBounds = bounds;
    DrawTextW(dc, text, -1, &textBounds, format | DT_NOPREFIX);
    RestoreDC(dc, saved);
}

} // namespace

/// @brief Return the immutable canonical installer palette.
/// @return Process-lifetime palette expressed as @c 0xRRGGBB values.
const InstallerBrandPalette &installerBrandPalette() noexcept {
    return kPalette;
}

/// @brief Calculate the WCAG contrast ratio between two RGB colors.
/// @param foreground Foreground encoded as @c 0xRRGGBB.
/// @param background Background encoded as @c 0xRRGGBB.
/// @return Contrast ratio from 1.0 through 21.0.
double installerContrastRatio(uint32_t foreground, uint32_t background) noexcept {
    const double foregroundLuminance = relativeLuminance(foreground);
    const double backgroundLuminance = relativeLuminance(background);
    const double lighter = std::max(foregroundLuminance, backgroundLuminance);
    const double darker = std::min(foregroundLuminance, backgroundLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

/// @brief Validate every required brand-palette contrast pairing.
/// @return @c true when body, muted, surface, and accent pairings meet their thresholds.
bool installerBrandPaletteMeetsContrast() noexcept {
    return installerContrastRatio(kPalette.text, kPalette.background) >= 7.0 &&
           installerContrastRatio(kPalette.textDim, kPalette.background) >= 4.5 &&
           installerContrastRatio(kPalette.text, kPalette.surface) >= 7.0 &&
           installerContrastRatio(kPalette.green, kPalette.background) >= 4.5 &&
           installerContrastRatio(kPalette.steel, kPalette.background) >= 4.5 &&
           installerContrastRatio(kPalette.teal, kPalette.background) >= 4.5 &&
           installerContrastRatio(kPalette.warning, kPalette.background) >= 4.5 &&
           installerContrastRatio(kPalette.danger, kPalette.background) >= 4.5;
}

/// @brief Resolve a semantic accent to its canonical palette color.
/// @param accent Semantic accent role.
/// @return Color encoded as @c 0xRRGGBB.
uint32_t installerAccentRgb(InstallerAccent accent) noexcept {
    switch (accent) {
        case InstallerAccent::Green:
            return kPalette.green;
        case InstallerAccent::Steel:
            return kPalette.steel;
        case InstallerAccent::Teal:
            return kPalette.teal;
        case InstallerAccent::Warning:
            return kPalette.warning;
        case InstallerAccent::Danger:
            return kPalette.danger;
    }
    return kPalette.teal;
}

/// @brief Query the current Windows high-contrast setting.
/// @return @c true when the operating system reports high contrast enabled.
bool installerHighContrastEnabled() noexcept {
    HIGHCONTRASTW contrast{static_cast<UINT>(sizeof(HIGHCONTRASTW))};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, static_cast<UINT>(sizeof(contrast)), &contrast, 0) &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

/// @brief Allocate the complete font and brush set for one normalized DPI.
/// @param dpi Native window DPI, normalized before resource allocation.
/// @throws std::runtime_error If any required GDI resource cannot be created.
InstallerThemeResources::InstallerThemeResources(UINT dpi)
    : dpi_(normalizeInstallerDpi(dpi)), highContrast_(installerHighContrastEnabled()) {
    try {
        const wchar_t *monoFace =
            fontFamilyAvailable(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
        bodyFont_ = createFont(dpi_, 10, FW_NORMAL, L"Segoe UI", DEFAULT_PITCH | FF_SWISS);
        bodyBoldFont_ = createFont(dpi_, 10, FW_SEMIBOLD, L"Segoe UI", DEFAULT_PITCH | FF_SWISS);
        smallFont_ = createFont(dpi_, 9, FW_NORMAL, L"Segoe UI", DEFAULT_PITCH | FF_SWISS);
        monoFont_ = createFont(dpi_, 9, FW_NORMAL, monoFace, FIXED_PITCH | FF_MODERN);
        monoBoldFont_ = createFont(dpi_, 10, FW_BOLD, monoFace, FIXED_PITCH | FF_MODERN);
        headingFont_ = createFont(dpi_, 22, FW_BOLD, monoFace, FIXED_PITCH | FF_MODERN);
        backgroundBrush_ = CreateSolidBrush(backgroundColor());
        raisedBrush_ = CreateSolidBrush(raisedColor());
        inputBrush_ = CreateSolidBrush(inputColor());
        if (!backgroundBrush_ || !raisedBrush_ || !inputBrush_)
            throw std::runtime_error("cannot create the Zanna installer interface brushes");
    } catch (...) {
        deleteFont(bodyFont_);
        deleteFont(bodyBoldFont_);
        deleteFont(smallFont_);
        deleteFont(monoFont_);
        deleteFont(monoBoldFont_);
        deleteFont(headingFont_);
        deleteBrush(backgroundBrush_);
        deleteBrush(raisedBrush_);
        deleteBrush(inputBrush_);
        throw;
    }
}

/// @brief Release every font and brush owned by this theme resource set.
InstallerThemeResources::~InstallerThemeResources() {
    deleteFont(bodyFont_);
    deleteFont(bodyBoldFont_);
    deleteFont(smallFont_);
    deleteFont(monoFont_);
    deleteFont(monoBoldFont_);
    deleteFont(headingFont_);
    deleteBrush(backgroundBrush_);
    deleteBrush(raisedBrush_);
    deleteBrush(inputBrush_);
}

/// @brief Transfer every GDI handle from another theme resource set.
/// @param other Resource set to empty.
InstallerThemeResources::InstallerThemeResources(InstallerThemeResources &&other) noexcept {
    *this = std::move(other);
}

/// @brief Release current handles and transfer ownership from another resource set.
/// @param other Resource set to empty.
/// @return This instance after ownership transfer.
InstallerThemeResources &InstallerThemeResources::operator=(
    InstallerThemeResources &&other) noexcept {
    if (this == &other)
        return *this;
    deleteFont(bodyFont_);
    deleteFont(bodyBoldFont_);
    deleteFont(smallFont_);
    deleteFont(monoFont_);
    deleteFont(monoBoldFont_);
    deleteFont(headingFont_);
    deleteBrush(backgroundBrush_);
    deleteBrush(raisedBrush_);
    deleteBrush(inputBrush_);

    dpi_ = std::exchange(other.dpi_, 96U);
    highContrast_ = std::exchange(other.highContrast_, false);
    bodyFont_ = std::exchange(other.bodyFont_, nullptr);
    bodyBoldFont_ = std::exchange(other.bodyBoldFont_, nullptr);
    smallFont_ = std::exchange(other.smallFont_, nullptr);
    monoFont_ = std::exchange(other.monoFont_, nullptr);
    monoBoldFont_ = std::exchange(other.monoBoldFont_, nullptr);
    headingFont_ = std::exchange(other.headingFont_, nullptr);
    backgroundBrush_ = std::exchange(other.backgroundBrush_, nullptr);
    raisedBrush_ = std::exchange(other.raisedBrush_, nullptr);
    inputBrush_ = std::exchange(other.inputBrush_, nullptr);
    return *this;
}

/// @brief Bound untrusted or invalid native DPI values before checked scaling.
/// @param dpi Candidate DPI.
/// @return Candidate value in the supported range, otherwise 96.
UINT normalizeInstallerDpi(UINT dpi) noexcept {
    constexpr UINT kMinimumSupportedDpi = 48U;
    constexpr UINT kMaximumSupportedDpi = 768U;
    return dpi >= kMinimumSupportedDpi && dpi <= kMaximumSupportedDpi ? dpi : 96U;
}

namespace {

/// @brief Compare an existing process-local window class with a requested definition.
/// @param requested Class identity and callback fields that must match.
/// @return @c true when Windows reports an equivalent registered class.
bool installerWindowClassMatches(const WNDCLASSEXW &requested) noexcept {
    WNDCLASSEXW existing{sizeof(existing)};
    if (!GetClassInfoExW(requested.hInstance, requested.lpszClassName, &existing))
        return false;
    return existing.hInstance == requested.hInstance &&
           existing.lpfnWndProc == requested.lpfnWndProc && existing.style == requested.style &&
           existing.cbClsExtra == requested.cbClsExtra &&
           existing.cbWndExtra == requested.cbWndExtra;
}

/// @brief Mutable state shared across child-window DPI enumeration.
struct ChildScaleContext {
    HWND parent{nullptr};
    UINT oldDpi{96U};
    UINT newDpi{96U};
    bool succeeded{true};
};

/// @brief Rescale one enumerated child window relative to its parent.
/// @param child Child window being repositioned.
/// @param parameter Pointer-sized reference to a ChildScaleContext.
/// @return @c TRUE to continue enumerating descendants after recording any failure.
BOOL CALLBACK rescaleInstallerChild(HWND child, LPARAM parameter) {
    auto &context = *reinterpret_cast<ChildScaleContext *>(parameter);
    RECT bounds{};
    if (!GetWindowRect(child, &bounds)) {
        context.succeeded = false;
        return TRUE;
    }
    POINT origin{bounds.left, bounds.top};
    if (!ScreenToClient(context.parent, &origin)) {
        context.succeeded = false;
        return TRUE;
    }
    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        context.succeeded = false;
        return TRUE;
    }
    if (!SetWindowPos(
            child,
            nullptr,
            MulDiv(origin.x, static_cast<int>(context.newDpi), static_cast<int>(context.oldDpi)),
            MulDiv(origin.y, static_cast<int>(context.newDpi), static_cast<int>(context.oldDpi)),
            MulDiv(width, static_cast<int>(context.newDpi), static_cast<int>(context.oldDpi)),
            MulDiv(height, static_cast<int>(context.newDpi), static_cast<int>(context.oldDpi)),
            SWP_NOACTIVATE | SWP_NOZORDER)) {
        context.succeeded = false;
    }
    return TRUE;
}

} // namespace

/// @brief Register an installer class or accept an already registered identical class.
/// @param windowClass Complete requested class definition.
/// @return Registered atom, one for a verified existing class, or zero on failure.
ATOM registerVerifiedInstallerWindowClass(const WNDCLASSEXW &windowClass) noexcept {
    if (windowClass.cbSize != sizeof(WNDCLASSEXW) || !windowClass.hInstance ||
        !windowClass.lpszClassName || !windowClass.lpfnWndProc) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const ATOM atom = RegisterClassExW(&windowClass);
    if (atom)
        return atom;
    const DWORD registrationError = GetLastError();
    if (registrationError != ERROR_CLASS_ALREADY_EXISTS ||
        !installerWindowClassMatches(windowClass)) {
        SetLastError(registrationError == ERROR_SUCCESS ? ERROR_INVALID_DATA : registrationError);
        return 0;
    }
    return 1;
}

/// @brief Rescale every descendant window for a per-monitor DPI transition.
/// @param parent Parent whose child coordinates are relative to its client area.
/// @param oldDpi DPI represented by current coordinates.
/// @param newDpi Target DPI.
/// @return @c true when all descendants were rescaled or no transition was needed.
bool rescaleInstallerChildWindows(HWND parent, UINT oldDpi, UINT newDpi) noexcept {
    if (!parent)
        return false;
    oldDpi = normalizeInstallerDpi(oldDpi);
    newDpi = normalizeInstallerDpi(newDpi);
    if (oldDpi == newDpi)
        return true;
    ChildScaleContext context{parent, oldDpi, newDpi, true};
    if (!EnumChildWindows(parent, rescaleInstallerChild, reinterpret_cast<LPARAM>(&context)))
        return false;
    return context.succeeded;
}

/// @brief Resolve the primary background for branded or high-contrast rendering.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::backgroundColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOW) : toColorRef(kPalette.background);
}

/// @brief Resolve the raised shell background for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::raisedColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOW) : toColorRef(kPalette.backgroundRaised);
}

/// @brief Resolve the interactive surface color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::surfaceColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_BTNFACE) : toColorRef(kPalette.surface);
}

/// @brief Resolve the editable input background for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::inputColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOW) : toColorRef(kPalette.input);
}

/// @brief Resolve the standard outline color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::borderColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : toColorRef(kPalette.border);
}

/// @brief Resolve the emphasized outline color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::borderBrightColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) : toColorRef(kPalette.borderBright);
}

/// @brief Resolve the primary text color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::textColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : toColorRef(kPalette.text);
}

/// @brief Resolve the secondary text color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::textDimColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : toColorRef(kPalette.textDim);
}

/// @brief Resolve the tertiary or disabled text color for the active accessibility mode.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::textFaintColor() const noexcept {
    return highContrast_ ? GetSysColor(COLOR_GRAYTEXT) : toColorRef(kPalette.textFaint);
}

/// @brief Resolve a semantic accent for branded or high-contrast rendering.
/// @param accent Accent role to resolve.
/// @return GDI @c COLORREF value.
COLORREF InstallerThemeResources::accentColor(InstallerAccent accent) const noexcept {
    return highContrast_ ? GetSysColor(COLOR_HIGHLIGHT) : toColorRef(installerAccentRgb(accent));
}

/// @brief Apply supported dark-title-bar and Explorer theme hints to a top-level window.
/// @param window Installer window to theme.
/// @param theme Active accessibility mode.
void applyInstallerWindowTheme(HWND window, const InstallerThemeResources &theme) noexcept {
    if (!window)
        return;
    if (!theme.highContrast()) {
        BOOL enabled = TRUE;
        constexpr DWORD kImmersiveDarkMode = 20;
        constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
        if (FAILED(DwmSetWindowAttribute(
                window, kImmersiveDarkMode, &enabled, static_cast<DWORD>(sizeof(enabled))))) {
            DwmSetWindowAttribute(window,
                                  kImmersiveDarkModeBefore20H1,
                                  &enabled,
                                  static_cast<DWORD>(sizeof(enabled)));
        }
        SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
    } else {
        SetWindowTheme(window, L"Explorer", nullptr);
    }
}

/// @brief Apply a native theme hint appropriate for an installer child control.
/// @param control Control to theme.
/// @param theme Active accessibility mode.
void applyInstallerControlTheme(HWND control, const InstallerThemeResources &theme) noexcept {
    if (!control)
        return;
    wchar_t className[32]{};
    const LONG_PTR style = GetWindowLongPtrW(control, GWL_STYLE);
    const LONG_PTR buttonType = style & BS_TYPEMASK;
    if (!theme.highContrast() && GetClassNameW(control, className, 32) > 0 &&
        std::wcscmp(className, L"Button") == 0 &&
        (buttonType == BS_GROUPBOX || buttonType == BS_RADIOBUTTON ||
         buttonType == BS_AUTORADIOBUTTON)) {
        SetWindowTheme(control, L"", L"");
        return;
    }
    SetWindowTheme(control, theme.highContrast() ? L"Explorer" : L"DarkMode_Explorer", nullptr);
}

/// @brief Paint the installer shell, compile rail, circuit field, and brand typography.
/// @param dc Destination device context.
/// @param bounds Full client bounds.
/// @param brandPanelWidth Optional left brand-panel width, or a nonpositive value to omit it.
/// @param theme Active brushes, fonts, colors, DPI, and accessibility mode.
void drawInstallerBackdrop(HDC dc,
                           const RECT &bounds,
                           int brandPanelWidth,
                           const InstallerThemeResources &theme) noexcept {
    if (!dc || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;
    FillRect(dc, &bounds, theme.raisedBrush());
    const int railHeight = std::max(2, scaled(4, theme.dpi()));
    RECT rail = bounds;
    rail.bottom = std::min(bounds.bottom, bounds.top + railHeight);
    const int third = std::max(1, static_cast<int>((rail.right - rail.left) / 3));
    const std::array<InstallerAccent, 3> accents = {
        InstallerAccent::Green, InstallerAccent::Steel, InstallerAccent::Teal};
    for (size_t index = 0; index < accents.size(); ++index) {
        RECT segment = rail;
        segment.left = rail.left + static_cast<int>(index) * third;
        segment.right = index + 1U == accents.size() ? rail.right : segment.left + third;
        HBRUSH brush = CreateSolidBrush(theme.accentColor(accents[index]));
        if (brush) {
            FillRect(dc, &segment, brush);
            DeleteObject(brush);
        }
    }

    if (brandPanelWidth <= 0)
        return;
    RECT brand = bounds;
    brand.right = std::min(bounds.right, bounds.left + brandPanelWidth);
    brand.top = rail.bottom;
    FillRect(dc, &brand, theme.backgroundBrush());
    drawCircuitField(dc, brand, theme);

    const int panelWidth = brand.right - brand.left;
    const int markSize = std::min(scaled(174, theme.dpi()), panelWidth - scaled(48, theme.dpi()));
    RECT mark{brand.left + (panelWidth - markSize) / 2,
              brand.top + scaled(54, theme.dpi()),
              brand.left + (panelWidth + markSize) / 2,
              brand.top + scaled(54, theme.dpi()) + markSize};
    drawInstallerBrandMark(dc, mark, theme);

    RECT nameBounds{brand.left + scaled(24, theme.dpi()),
                    mark.bottom + scaled(30, theme.dpi()),
                    brand.right - scaled(24, theme.dpi()),
                    mark.bottom + scaled(70, theme.dpi())};
    drawTextLine(dc,
                 nameBounds,
                 L"ZANNA",
                 theme.headingFont(),
                 theme.textColor(),
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT platformBounds{nameBounds.left,
                        nameBounds.bottom,
                        nameBounds.right,
                        nameBounds.bottom + scaled(30, theme.dpi())};
    drawTextLine(dc,
                 platformBounds,
                 L"DEVELOPER PLATFORM",
                 theme.monoBoldFont(),
                 theme.accentColor(InstallerAccent::Green),
                 DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    RECT taglineBounds{brand.left + scaled(18, theme.dpi()),
                       platformBounds.bottom + scaled(22, theme.dpi()),
                       brand.right - scaled(18, theme.dpi()),
                       platformBounds.bottom + scaled(70, theme.dpi())};
    drawTextLine(dc,
                 taglineBounds,
                 L"CODE. CREATE.\r\nCOMPILE. CONQUER.",
                 theme.monoFont(),
                 theme.textDimColor(),
                 DT_CENTER | DT_WORDBREAK);
}

/// @brief Draw the three-piece vector Z mark.
/// @param dc Destination device context.
/// @param bounds Rectangle enclosing the mark.
/// @param theme Active colors, DPI, and accessibility mode.
void drawInstallerBrandMark(HDC dc,
                            const RECT &bounds,
                            const InstallerThemeResources &theme) noexcept {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 8 || height <= 8)
        return;
    const int saved = SaveDC(dc);
    if (saved == 0)
        return;
    const int bar = std::max(3, height / 5);
    const int slant = std::max(2, width / 10);
    const int inset = std::max(2, width / 8);

    const std::array<POINT, 4> top = {
        POINT{bounds.left + inset, bounds.top},
        POINT{bounds.right, bounds.top},
        POINT{bounds.right - slant, bounds.top + bar},
        POINT{bounds.left, bounds.top + bar},
    };
    const int diagonalWidth = std::max(5, width / 12);
    const std::array<POINT, 4> diagonal = {
        POINT{bounds.right - slant, bounds.top + bar - 1},
        POINT{bounds.right - slant - diagonalWidth, bounds.top + bar - 1},
        POINT{bounds.left + slant, bounds.bottom - bar + 1},
        POINT{bounds.left + slant + diagonalWidth, bounds.bottom - bar + 1},
    };
    const std::array<POINT, 4> bottom = {
        POINT{bounds.left + slant, bounds.bottom - bar},
        POINT{bounds.right, bounds.bottom - bar},
        POINT{bounds.right - inset, bounds.bottom},
        POINT{bounds.left, bounds.bottom},
    };

    const std::array<std::pair<const POINT *, InstallerAccent>, 3> shapes = {
        std::pair{top.data(), InstallerAccent::Green},
        std::pair{diagonal.data(), InstallerAccent::Steel},
        std::pair{bottom.data(), InstallerAccent::Teal},
    };
    for (const auto &[points, accent] : shapes) {
        HBRUSH brush = CreateSolidBrush(theme.accentColor(accent));
        HPEN pen = CreatePen(PS_SOLID, 1, theme.borderBrightColor());
        if (brush && pen) {
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            Polygon(dc, points, 4);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
        }
        if (pen)
            DeleteObject(pen);
        if (brush)
            DeleteObject(brush);
    }
    RestoreDC(dc, saved);
}

/// @brief Paint an accessible owner-drawn action button and its optional detail line.
/// @param item Windows owner-draw state, control handle, device context, and bounds.
/// @param accent Semantic accent used for the leading rail and focus outline.
/// @param theme Active fonts, colors, DPI, and accessibility mode.
void drawInstallerActionButton(const DRAWITEMSTRUCT &item,
                               InstallerAccent accent,
                               const InstallerThemeResources &theme) noexcept {
    if (!item.hDC || !item.hwndItem)
        return;
    const int saved = SaveDC(item.hDC);
    if (saved == 0)
        return;
    RECT bounds = item.rcItem;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0U;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0U;
    const bool focused = (item.itemState & ODS_FOCUS) != 0U;
    COLORREF fill = theme.surfaceColor();
    if (pressed && !theme.highContrast())
        fill = toColorRef(kPalette.surfaceHover);
    if (theme.highContrast() && pressed)
        fill = GetSysColor(COLOR_HIGHLIGHT);

    HBRUSH fillBrush = CreateSolidBrush(fill);
    HPEN borderPen = CreatePen(
        PS_SOLID, focused ? 2 : 1, focused ? theme.accentColor(accent) : theme.borderColor());
    if (fillBrush && borderPen) {
        HGDIOBJ oldBrush = SelectObject(item.hDC, fillBrush);
        HGDIOBJ oldPen = SelectObject(item.hDC, borderPen);
        const int radius = std::max(4, scaled(8, theme.dpi()));
        RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
        SelectObject(item.hDC, oldPen);
        SelectObject(item.hDC, oldBrush);
    }
    if (borderPen)
        DeleteObject(borderPen);
    if (fillBrush)
        DeleteObject(fillBrush);

    RECT accentBounds = bounds;
    accentBounds.right = std::min(bounds.right, bounds.left + std::max(3, scaled(5, theme.dpi())));
    accentBounds.top += scaled(7, theme.dpi());
    accentBounds.bottom -= scaled(7, theme.dpi());
    HBRUSH accentBrush = CreateSolidBrush(theme.accentColor(accent));
    if (accentBrush) {
        FillRect(item.hDC, &accentBounds, accentBrush);
        DeleteObject(accentBrush);
    }

    wchar_t text[512]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    wchar_t *detail = std::wcschr(text, L'\n');
    if (detail) {
        *detail = L'\0';
        ++detail;
    }
    const int left = bounds.left + scaled(20, theme.dpi());
    const int right = bounds.right - scaled(16, theme.dpi());
    COLORREF primaryText = disabled ? theme.textFaintColor() : theme.textColor();
    COLORREF secondaryText = disabled ? theme.textFaintColor() : theme.textDimColor();
    if (theme.highContrast() && pressed && !disabled)
        primaryText = secondaryText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    if (detail && *detail) {
        RECT titleBounds{
            left, bounds.top + scaled(9, theme.dpi()), right, bounds.top + scaled(31, theme.dpi())};
        drawTextLine(item.hDC,
                     titleBounds,
                     text,
                     theme.monoBoldFont(),
                     primaryText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT detailBounds{left, titleBounds.bottom, right, bounds.bottom - scaled(7, theme.dpi())};
        drawTextLine(item.hDC,
                     detailBounds,
                     detail,
                     theme.smallFont(),
                     secondaryText,
                     DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else {
        RECT textBounds = bounds;
        textBounds.left = left;
        textBounds.right = right;
        drawTextLine(item.hDC,
                     textBounds,
                     text,
                     theme.bodyBoldFont(),
                     primaryText,
                     DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (focused && theme.highContrast()) {
        InflateRect(&bounds, -scaled(3, theme.dpi()), -scaled(3, theme.dpi()));
        DrawFocusRect(item.hDC, &bounds);
    }
    RestoreDC(item.hDC, saved);
}

/// @brief Configure control text and background colors for a @c WM_CTLCOLOR* request.
/// @param message Control-color message identifying editable or transparent behavior.
/// @param dc Device context to configure.
/// @param control Control requesting colors, used to select enabled text color.
/// @param theme Active colors, brushes, and accessibility mode.
/// @return Borrowed input or raised brush, or null for an invalid device context.
HBRUSH colorInstallerControl(UINT message,
                             HDC dc,
                             HWND control,
                             const InstallerThemeResources &theme) noexcept {
    if (!dc)
        return nullptr;
    const bool enabled = !control || IsWindowEnabled(control) != FALSE;
    SetTextColor(dc, enabled ? theme.textColor() : theme.textFaintColor());
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, theme.inputColor());
        return theme.inputBrush();
    }
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, theme.raisedColor());
    return theme.raisedBrush();
}

} // namespace zanna::installer
