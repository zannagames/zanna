//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerTheme.hpp
// Purpose: Declare the native Zanna Games visual system used by Windows setup.
//
// Key invariants:
//   - The default palette matches the dark-only zannagames.com brand surface.
//   - High-contrast mode replaces branded colors with Windows system colors.
//   - The Z mark is drawn from native vector primitives at the active DPI.
//   - Painting and resource ownership introduce no external runtime dependency.
//
// Ownership/Lifetime:
//   - InstallerThemeResources owns its GDI fonts and brushes.
//   - Drawing helpers borrow caller-owned device contexts and rectangles.
//
// Links: WindowsInstallerTheme.cpp, WindowsInstallerBrandDialog.cpp,
//        misc/plans/zannastudio/02-brand-reface.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <windows.h>

#include <cstdint>

namespace zanna::installer {

/// @brief RGB colors shared with the website and Zanna Studio dark theme.
struct InstallerBrandPalette {
    uint32_t background;
    uint32_t backgroundRaised;
    uint32_t surface;
    uint32_t surfaceHover;
    uint32_t input;
    uint32_t border;
    uint32_t borderBright;
    uint32_t text;
    uint32_t textDim;
    uint32_t textFaint;
    uint32_t green;
    uint32_t greenDim;
    uint32_t steel;
    uint32_t teal;
    uint32_t tealDeep;
    uint32_t warning;
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
const InstallerBrandPalette &installerBrandPalette() noexcept;

/// @brief Calculate the WCAG relative-luminance contrast ratio of two RGB colors.
double installerContrastRatio(uint32_t foreground, uint32_t background) noexcept;

/// @brief Verify the palette's body, muted, button, and focus contrast contracts.
bool installerBrandPaletteMeetsContrast() noexcept;

/// @brief Convert a semantic accent to a canonical RGB value.
uint32_t installerAccentRgb(InstallerAccent accent) noexcept;

/// @brief Return whether Windows high-contrast presentation is active.
bool installerHighContrastEnabled() noexcept;

/// @brief Own the DPI-specific fonts and brushes for one installer window.
class InstallerThemeResources {
  public:
    explicit InstallerThemeResources(UINT dpi);
    ~InstallerThemeResources();

    InstallerThemeResources(const InstallerThemeResources &) = delete;
    InstallerThemeResources &operator=(const InstallerThemeResources &) = delete;
    InstallerThemeResources(InstallerThemeResources &&other) noexcept;
    InstallerThemeResources &operator=(InstallerThemeResources &&other) noexcept;

    UINT dpi() const noexcept {
        return dpi_;
    }

    bool highContrast() const noexcept {
        return highContrast_;
    }

    HFONT bodyFont() const noexcept {
        return bodyFont_;
    }

    HFONT bodyBoldFont() const noexcept {
        return bodyBoldFont_;
    }

    HFONT smallFont() const noexcept {
        return smallFont_;
    }

    HFONT monoFont() const noexcept {
        return monoFont_;
    }

    HFONT monoBoldFont() const noexcept {
        return monoBoldFont_;
    }

    HFONT headingFont() const noexcept {
        return headingFont_;
    }

    HBRUSH backgroundBrush() const noexcept {
        return backgroundBrush_;
    }

    HBRUSH raisedBrush() const noexcept {
        return raisedBrush_;
    }

    HBRUSH inputBrush() const noexcept {
        return inputBrush_;
    }

    COLORREF backgroundColor() const noexcept;
    COLORREF raisedColor() const noexcept;
    COLORREF surfaceColor() const noexcept;
    COLORREF inputColor() const noexcept;
    COLORREF borderColor() const noexcept;
    COLORREF borderBrightColor() const noexcept;
    COLORREF textColor() const noexcept;
    COLORREF textDimColor() const noexcept;
    COLORREF textFaintColor() const noexcept;
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
UINT normalizeInstallerDpi(UINT dpi) noexcept;

/// @brief Register a process-local installer class or verify an identical existing class.
ATOM registerVerifiedInstallerWindowClass(const WNDCLASSEXW &windowClass) noexcept;

/// @brief Rescale every direct and nested child control for a per-monitor DPI transition.
bool rescaleInstallerChildWindows(HWND parent, UINT oldDpi, UINT newDpi) noexcept;

/// @brief Apply supported dark title-bar and native-control hints.
void applyInstallerWindowTheme(HWND window, const InstallerThemeResources &theme) noexcept;

/// @brief Apply the appropriate native theme hint to a child control.
void applyInstallerControlTheme(HWND control, const InstallerThemeResources &theme) noexcept;

/// @brief Paint the dark shell, three-color compile rail, and optional brand panel.
void drawInstallerBackdrop(HDC dc,
                           const RECT &bounds,
                           int brandPanelWidth,
                           const InstallerThemeResources &theme) noexcept;

/// @brief Draw the green/steel/teal vector Z inside @p bounds.
void drawInstallerBrandMark(HDC dc,
                            const RECT &bounds,
                            const InstallerThemeResources &theme) noexcept;

/// @brief Draw one accessible native owner-drawn action button.
void drawInstallerActionButton(const DRAWITEMSTRUCT &item,
                               InstallerAccent accent,
                               const InstallerThemeResources &theme) noexcept;

/// @brief Apply branded or system control colors and return the matching brush.
HBRUSH colorInstallerControl(UINT message,
                             HDC dc,
                             HWND control,
                             const InstallerThemeResources &theme) noexcept;

} // namespace zanna::installer
