//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares terminal color, style, cell, and differential screen-buffer types.
/// @details Widgets paint a current 2D cell grid, while ScreenBuffer retains a
///          previous snapshot and reports minimal horizontal change spans for
///          the ANSI renderer.
//
// The rendering pipeline works as follows:
//   1. ScreenBuffer::clear() resets all cells to a background style
//   2. Widgets paint characters and styles into the buffer via at()
//   3. snapshotPrev() saves the current state for diffing
//   4. After the next paint, computeDiff() identifies changed regions
//   5. The Renderer emits ANSI sequences only for changed spans
//
// Key invariants:
//   - Cell coordinates are 0-based (y=row, x=column).
//   - at() performs no bounds checking; callers must respect dimensions.
//   - snapshotPrev() must be called before computeDiff() for valid results.
//   - Wide characters (width > 1) occupy their cell; trailing cells are
//     the caller's responsibility.
//
// Ownership: ScreenBuffer owns its cell grids (current and previous)
// by value via std::vector. RGBA, Style, and Cell are trivial value types.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <vector>

namespace zanna::tui::render {
/// @brief 32-bit RGBA color value for terminal cell styling.
/// @details Represents a color with red, green, blue, and alpha channels.
///          Alpha defaults to 255 (fully opaque). Used for both foreground
///          and background colors in the Style struct.
struct RGBA {
    uint8_t r{0};   ///< Red channel in the inclusive range 0..255.
    uint8_t g{0};   ///< Green channel in the inclusive range 0..255.
    uint8_t b{0};   ///< Blue channel in the inclusive range 0..255.
    uint8_t a{255}; ///< Alpha channel; 255 is fully opaque.
};

/// @brief Compare two colors channel by channel.
/// @param a First color.
/// @param b Second color.
/// @return true when all RGBA channels match.
bool operator==(const RGBA &a, const RGBA &b);

/// @brief Test two colors for channel inequality.
/// @param a First color.
/// @param b Second color.
/// @return true when at least one RGBA channel differs.
bool operator!=(const RGBA &a, const RGBA &b);

/// @brief Bitflags for terminal text attributes applied to cells.
/// @details Can be combined with bitwise OR to apply multiple attributes
///          simultaneously (e.g., Bold | Italic for bold italic text).
///          These map to standard ANSI/VT text attribute codes.
enum Attr : uint16_t {
    AttrNone = 0,      ///< No text attributes.
    Bold = 1 << 0,     ///< Increased intensity.
    Faint = 1 << 1,    ///< Decreased intensity.
    Italic = 1 << 2,   ///< Italic text.
    Underline = 1 << 3, ///< Underlined text.
    Blink = 1 << 4,    ///< Blinking text where supported.
    Reverse = 1 << 5,  ///< Swapped foreground and background.
    Invisible = 1 << 6, ///< Concealed glyphs.
    Strike = 1 << 7    ///< Strikethrough text.
};

/// @brief Visual style applied to a terminal cell, combining colors and attributes.
/// @details Contains foreground color, background color, and text attribute flags.
///          Styles are compared for equality during diff computation to minimize
///          terminal escape sequence output.
struct Style {
    RGBA fg{};        ///< Foreground glyph color.
    RGBA bg{};        ///< Background cell color.
    uint16_t attrs{0}; ///< Bitwise combination of @ref Attr values.
};

/// @brief Compare two terminal styles.
/// @param a First style.
/// @param b Second style.
/// @return true when foreground, background, and attributes match.
bool operator==(const Style &a, const Style &b);

/// @brief Test two terminal styles for inequality.
/// @param a First style.
/// @param b Second style.
/// @return true when any style component differs.
bool operator!=(const Style &a, const Style &b);

/// @brief Single character cell in the screen buffer with style and display width.
/// @details Represents one position in the terminal grid. The character is stored
///          as a UTF-32 code point to support the full Unicode range. Width indicates
///          how many terminal columns the character occupies (1 for most characters,
///          2 for wide CJK characters, 0 for combining marks).
struct Cell {
    char32_t ch{U' '}; ///< Unicode code point displayed in this cell.
    Style style{};     ///< Colors and attributes applied to the glyph.
    uint8_t width{1};  ///< Terminal column width of the glyph.
};

/// @brief Compare two screen cells.
/// @param a First cell.
/// @param b Second cell.
/// @return true when glyph, style, and display width match.
bool operator==(const Cell &a, const Cell &b);

/// @brief Test two screen cells for inequality.
/// @param a First cell.
/// @param b Second cell.
/// @return true when any cell component differs.
bool operator!=(const Cell &a, const Cell &b);

/// @brief 2D grid of styled character cells with differential update support.
/// @details Provides the primary rendering surface for the TUI widget system.
///          Widgets paint into the buffer via at(), and the Renderer uses
///          snapshotPrev()/computeDiff() to emit only the changed portions
///          to the terminal, minimizing I/O overhead.
class ScreenBuffer {
  public:
    /// @brief Describes a contiguous horizontal span of changed cells within a row.
    /// @details Used by computeDiff() to report regions that need to be redrawn.
    ///          The Renderer iterates these spans to emit targeted ANSI sequences.
    struct DiffSpan {
        int row{0}; ///< Zero-based row containing the changes.
        int x0{0};  ///< Inclusive first changed column.
        int x1{0};  ///< Exclusive column after the changed span.
    };

    /// @brief Resize buffer to given rows and columns.
    /// @param rows New nonnegative row count.
    /// @param cols New nonnegative column count.
    void resize(int rows, int cols);

    /// @brief Access cell at position (y, x).
    /// @param y Zero-based row.
    /// @param x Zero-based column.
    /// @return Mutable cell reference; coordinates must be in bounds.
    [[nodiscard]] Cell &at(int y, int x);

    /// @brief Const access to cell at position (y, x).
    /// @param y Zero-based row.
    /// @param x Zero-based column.
    /// @return Const cell reference; coordinates must be in bounds.
    [[nodiscard]] const Cell &at(int y, int x) const;

    /// @brief Fill all cells with spaces using the given style.
    /// @param style Style assigned to every cleared cell.
    void clear(const Style &style);

    /// @brief Fill a rectangular region with the specified character and style.
    /// @param x Left column of the rectangle.
    /// @param y Top row of the rectangle.
    /// @param w Width of the rectangle.
    /// @param h Height of the rectangle.
    /// @param ch Character to fill with (defaults to space).
    /// @param style Optional style to apply (cell styles unchanged if nullptr).
    void fillRect(int x, int y, int w, int h, char32_t ch = U' ', const Style *style = nullptr);

    /// @brief Snapshot current buffer into previous state for diffing.
    void snapshotPrev();

    /// @brief Compute differences against previous snapshot.
    /// @param outSpans Output vector receiving change spans per row.
    void computeDiff(std::vector<DiffSpan> &outSpans) const;

  private:
    int rows_{0};                 ///< Current grid height.
    int cols_{0};                 ///< Current grid width.
    std::vector<Cell> cells_{};   ///< Current frame cells in row-major order.
    std::vector<Cell> prev_{};    ///< Previous snapshot cells in row-major order.
};

} // namespace zanna::tui::render
