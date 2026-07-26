//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the ANSI terminal renderer for Zanna TUI screen-buffer diffs.
/// @details Renderer is the final pipeline stage, translating abstract changed
///          cells into cursor movement, style changes, glyph writes, and flushes
///          on a borrowed terminal I/O backend.
//
// The Renderer maintains minimal state: the current cursor position and
// active text style. It only emits escape sequences when the style or
// position changes, minimizing terminal I/O bandwidth.
//
// Rendering modes:
//   - truecolor (24-bit): Uses SGR 38;2;r;g;b and 48;2;r;g;b sequences
//   - 256-color (default): Maps RGBA colors to the nearest 256-color index
//
// Key invariants:
//   - draw() processes DiffSpans from the ScreenBuffer to emit only changes.
//   - Cursor position is tracked to avoid redundant cursor movement sequences.
//   - Style state is tracked to avoid redundant SGR attribute sequences.
//
// Ownership: Renderer borrows a TermIO reference for output. It holds no
// other resources.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tui/render/screen.hpp"
#include "tui/term/term_io.hpp"

namespace zanna::tui::render {

/// @brief Converts ScreenBuffer diffs into ANSI escape sequences for terminal output.
/// @details The final stage of the TUI rendering pipeline. Computes differential
///          updates by processing DiffSpan records from the ScreenBuffer, emitting
///          only the escape sequences needed to update changed cells. Tracks cursor
///          position and active style to minimize redundant output.
class Renderer {
  public:
    /// @brief Construct renderer targeting a TermIO.
    /// @param tio Terminal I/O sink used for writes and flushes.
    /// @param truecolor Emit 24-bit color codes when true; otherwise 256-color.
    explicit Renderer(::zanna::tui::term::TermIO &tio, bool truecolor = false);

    /// @brief Draw changed spans from screen buffer to terminal.
    /// @param sb Screen buffer with current and previous state for diffing.
    void draw(const ScreenBuffer &sb);

    /// @brief Update terminal style if different from current style.
    /// @param style Desired style to apply.
    void setStyle(Style style);

    /// @brief Move cursor to given coordinates if not already there.
    /// @param y Row index starting at 0.
    /// @param x Column index starting at 0.
    void moveCursor(int y, int x);

  private:
    ::zanna::tui::term::TermIO &tio_; ///< Borrowed terminal output backend.
    Style currentStyle_{};            ///< Style currently active in the terminal.
    int cursorY_{-1};                 ///< Tracked zero-based cursor row, or -1 if unknown.
    int cursorX_{-1};                 ///< Tracked zero-based cursor column, or -1 if unknown.
    bool truecolor_{false};            ///< Whether to emit 24-bit rather than indexed colors.
};

} // namespace zanna::tui::render
