//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file WindowsInstallerTheme.hpp
/// @brief Declares the native Zanna Games visual system used by Windows setup.
///
/// The default palette matches the dark-only Zanna Games brand surface, while high-contrast mode
/// substitutes Windows system colors. The vector Z mark and every layout metric scale at the
/// active DPI without external runtime dependencies.
///
/// InstallerThemeResources owns its GDI fonts and brushes. Drawing helpers borrow caller-owned
/// device contexts, windows, and rectangles.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <windows.h>

#include <cstdint>

namespace zanna::installer {

/// @brief Semantic RGB colors shared with the website and Zanna Studio dark theme.
struct InstallerBrandPalette {
    /// @brief Primary window background.
    uint32_t background;
    /// @brief Raised shell background.
    uint32_t backgroundRaised;
    /// @brief Interactive surface background.
    uint32_t surface;
    /// @brief Hovered surface background.
    uint32_t surfaceHover;
    /// @brief Editable input background.
    uint32_t input;
    /// @brief Standard separator and outline.
    uint32_t border;
    /// @brief Emphasized border and focus ring.
    uint32_t borderBright;
    /// @brief Primary body text.
    uint32_t text;
    /// @brief Secondary body text.
    uint32_t textDim;
    /// @brief Tertiary decorative text.
    uint32_t textFaint;
    /// @brief Primary green brand accent.
    uint32_t green;
    /// @brief Dark green accent variant.
    uint32_t greenDim;
    /// @brief Steel brand accent.
    uint32_t steel;
    /// @brief Teal brand accent.
    uint32_t teal;
    /// @brief Deep teal accent variant.
    uint32_t tealDeep;
    /// @brief Warning-state accent.
    uint32_t warning;
    /// @brief Destructive or error-state accent.
    uint32_t danger;
};

/// @brief Semantic accent applied to an installer action card.
enum class InstallerAccent {
    Green,
    Steel,
    Teal,
    Warning,
    Danger,
};

/// @brief Return the canonical Zanna Games installer palette.
/// @return Process-lifetime immutable palette expressed as @c 0xRRGGBB values.
const InstallerBrandPalette &installerBrandPalette() noexcept;

/// @brief Calculate the WCAG relative-luminance contrast ratio of two RGB colors.
/// @param foreground Foreground color encoded as @c 0xRRGGBB.
/// @param background Background color encoded as @c 0xRRGGBB.
/// @return Contrast ratio from 1.0 through 21.0.
double installerContrastRatio(uint32_t foreground, uint32_t background) noexcept;

/// @brief Verify the palette's body, muted, button, and focus contrast contracts.
/// @return @c true when every required palette pairing meets its WCAG threshold.
bool installerBrandPaletteMeetsContrast() noexcept;

/// @brief Convert a semantic accent to a canonical RGB value.
/// @param accent Semantic action or state accent.
/// @return Canonical palette value encoded as @c 0xRRGGBB.
uint32_t installerAccentRgb(InstallerAccent accent) noexcept;

/// @brief Return whether Windows high-contrast presentation is active.
/// @return @c true when the operating system reports high-contrast mode enabled.
bool installerHighContrastEnabled() noexcept;

/// @brief Own the DPI-specific fonts and brushes for one installer window.
class InstallerThemeResources {
  public:
    /// @brief Allocate fonts and solid brushes for a normalized window DPI.
    /// @param dpi Native DPI reported for the owning installer window.
    /// @throws std::runtime_error If a required GDI resource cannot be created.
    explicit InstallerThemeResources(UINT dpi);

    /// @brief Release every owned font and brush.
    ~InstallerThemeResources();

    /// @brief Theme resources have unique GDI ownership and cannot be copied.
    InstallerThemeResources(const InstallerThemeResources &) = delete;

    /// @brief Theme resources have unique GDI ownership and cannot be copy-assigned.
    /// @return This resource set; the operation is deleted.
    InstallerThemeResources &operator=(const InstallerThemeResources &) = delete;

    /// @brief Transfer all GDI ownership from another resource set.
    /// @param other Resource set to empty.
    InstallerThemeResources(InstallerThemeResources &&other) noexcept;

    /// @brief Release current resources and transfer ownership from another set.
    /// @param other Resource set to empty.
    /// @return This resource set after the transfer.
    InstallerThemeResources &operator=(InstallerThemeResources &&other) noexcept;

    /// @brief Return the normalized DPI used to allocate and scale resources.
    /// @return DPI in the supported installer range.
    UINT dpi() const noexcept {
        return dpi_;
    }

    /// @brief Report whether this resource set uses Windows high-contrast colors.
    /// @return @c true when high-contrast presentation was active at construction.
    bool highContrast() const noexcept {
        return highContrast_;
    }

    /// @brief Return the regular body font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT bodyFont() const noexcept {
        return bodyFont_;
    }

    /// @brief Return the semibold body font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT bodyBoldFont() const noexcept {
        return bodyBoldFont_;
    }

    /// @brief Return the small-caption font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT smallFont() const noexcept {
        return smallFont_;
    }

    /// @brief Return the regular monospaced font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT monoFont() const noexcept {
        return monoFont_;
    }

    /// @brief Return the bold monospaced font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT monoBoldFont() const noexcept {
        return monoBoldFont_;
    }

    /// @brief Return the large heading font.
    /// @return Borrowed GDI font handle owned by this instance.
    HFONT headingFont() const noexcept {
        return headingFont_;
    }

    /// @brief Return the primary background brush.
    /// @return Borrowed GDI brush owned by this instance.
    HBRUSH backgroundBrush() const noexcept {
        return backgroundBrush_;
    }

    /// @brief Return the raised-panel background brush.
    /// @return Borrowed GDI brush owned by this instance.
    HBRUSH raisedBrush() const noexcept {
        return raisedBrush_;
    }

    /// @brief Return the editable-input background brush.
    /// @return Borrowed GDI brush owned by this instance.
    HBRUSH inputBrush() const noexcept {
        return inputBrush_;
    }

    /// @brief Resolve the primary background color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF backgroundColor() const noexcept;

    /// @brief Resolve the raised-panel color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF raisedColor() const noexcept;

    /// @brief Resolve the interactive-surface color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF surfaceColor() const noexcept;

    /// @brief Resolve the editable-input color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF inputColor() const noexcept;

    /// @brief Resolve the standard border color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF borderColor() const noexcept;

    /// @brief Resolve the emphasized border color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF borderBrightColor() const noexcept;

    /// @brief Resolve the primary text color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF textColor() const noexcept;

    /// @brief Resolve the secondary text color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF textDimColor() const noexcept;

    /// @brief Resolve the tertiary text color for the active accessibility mode.
    /// @return GDI @c COLORREF value.
    COLORREF textFaintColor() const noexcept;

    /// @brief Resolve a semantic accent for the active accessibility mode.
    /// @param accent Accent role to resolve.
    /// @return GDI @c COLORREF value.
    COLORREF accentColor(InstallerAccent accent) const noexcept;

  private:
    UINT dpi_{96};
    bool highContrast_{false};
    HFONT bodyFont_{nullptr};
    HFONT bodyBoldFont_{nullptr};
    HFONT smallFont_{nullptr};
    HFONT monoFont_{nullptr};
    HFONT monoBoldFont_{nullptr};
    HFONT headingFont_{nullptr};
    HBRUSH backgroundBrush_{nullptr};
    HBRUSH raisedBrush_{nullptr};
    HBRUSH inputBrush_{nullptr};
};

/// @brief Normalize a native DPI value before any checked pixel scaling.
/// @param dpi Candidate DPI reported by Windows.
/// @return @p dpi when supported, otherwise the 96-DPI baseline.
UINT normalizeInstallerDpi(UINT dpi) noexcept;

/// @brief Register a process-local installer class or verify an identical existing class.
/// @param windowClass Fully initialized class description.
/// @return Registered class atom, one for an identical existing class, or zero on failure.
/// @note On failure, the function preserves or assigns a meaningful Win32 last-error code.
ATOM registerVerifiedInstallerWindowClass(const WNDCLASSEXW &windowClass) noexcept;

/// @brief Rescale every direct and nested child control for a per-monitor DPI transition.
/// @param parent Parent installer window whose descendants are repositioned.
/// @param oldDpi DPI used by current child coordinates.
/// @param newDpi DPI to which coordinates and dimensions are scaled.
/// @return @c true when every descendant was rescaled or no DPI change was needed.
bool rescaleInstallerChildWindows(HWND parent, UINT oldDpi, UINT newDpi) noexcept;

/// @brief Apply supported dark title-bar and native-control hints.
/// @param window Top-level installer window to theme.
/// @param theme Active theme resources and accessibility mode.
void applyInstallerWindowTheme(HWND window, const InstallerThemeResources &theme) noexcept;

/// @brief Apply the appropriate native theme hint to a child control.
/// @param control Child control to theme.
/// @param theme Active theme resources and accessibility mode.
void applyInstallerControlTheme(HWND control, const InstallerThemeResources &theme) noexcept;

/// @brief Paint the dark shell, three-color compile rail, and optional brand panel.
/// @param dc Destination device context.
/// @param bounds Client rectangle to fill.
/// @param brandPanelWidth Width of the optional left brand panel in pixels.
/// @param theme Active fonts, colors, brushes, DPI, and accessibility mode.
void drawInstallerBackdrop(HDC dc,
                           const RECT &bounds,
                           int brandPanelWidth,
                           const InstallerThemeResources &theme) noexcept;

/// @brief Draw the green/steel/teal vector Z inside @p bounds.
/// @param dc Destination device context.
/// @param bounds Rectangle enclosing the mark.
/// @param theme Active colors, DPI, and accessibility mode.
void drawInstallerBrandMark(HDC dc,
                            const RECT &bounds,
                            const InstallerThemeResources &theme) noexcept;

/// @brief Draw one accessible native owner-drawn action button.
/// @param item Owner-draw state and destination rectangle supplied by Windows.
/// @param accent Semantic button accent.
/// @param theme Active fonts, colors, DPI, and accessibility mode.
void drawInstallerActionButton(const DRAWITEMSTRUCT &item,
                               InstallerAccent accent,
                               const InstallerThemeResources &theme) noexcept;

/// @brief Apply branded or system control colors and return the matching brush.
/// @param message @c WM_CTLCOLOR* message identifying the control category.
/// @param dc Device context whose text and background colors are configured.
/// @param control Control requesting colors; currently reserved for category-specific behavior.
/// @param theme Active colors, brushes, and accessibility mode.
/// @return Borrowed brush that Windows should use to paint the control background.
HBRUSH colorInstallerControl(UINT message,
                             HDC dc,
                             HWND control,
                             const InstallerThemeResources &theme) noexcept;

} // namespace zanna::installer
