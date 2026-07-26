//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the ListView widget used in the Zanna terminal UI toolkit.  The
// widget renders a vertical list of strings, tracks cursor movement, and manages
// single- or multi-selection depending on modifier keys supplied with navigation
// events.  Rendering behaviour is customisable via a callback while sensible
// defaults provide a minimal arrow-and-text presentation.
//
// Invariants:
//   * The `selected_` bitset always mirrors the length of `items_` so selection
//     operations remain index-safe.
//   * The cursor remains within `[0, items_.size())` whenever the list is
//     non-empty, enabling callers to rely on it for highlighting.
// Ownership:
//   * ListView stores items by value and borrows a theme reference so multiple
//     widgets can share styling without redundant copies.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides rendering and event handling for the ListView widget.
/// @details The translation unit wires together navigation logic, selection
///          updates, and the default renderer that paints the list into a screen
///          buffer.  Modifier-aware selection rules mirror conventions from
///          desktop file browsers, making the widget intuitive to use.

#include "tui/widgets/list_view.hpp"
#include "tui/render/screen.hpp"

#include <algorithm>

namespace zanna::tui::widgets {

/// @brief Initialise a list view with items and a shared theme reference.
/// @details Copies @p items into the internal storage, primes the selection
///          vector with a single active entry when the list is non-empty, and
///          installs the default renderer.  Clients can later swap the renderer
///          via @ref setRenderer() to alter presentation without replacing the
///          control.
/// @param items Initial row labels, moved into the widget.
/// @param theme Borrowed theme that must outlive the list view.
ListView::ListView(std::vector<std::string> items, const style::Theme &theme)
    : items_(std::move(items)), theme_(theme) {
    selected_.resize(items_.size(), false);
    if (!items_.empty()) {
        selected_[0] = true;
    }
    /// @brief Render one list item with the built-in row renderer.
    /// @param sb Screen buffer receiving the row.
    /// @param row Absolute screen row.
    /// @param item Item label.
    /// @param selected Whether the item is selected.
    /// @param theme Theme argument supplied by the renderer interface.
    renderer_ = [this](render::ScreenBuffer &sb,
                       int row,
                       const std::string &item,
                       bool selected,
                       const style::Theme &) { defaultRender(sb, row, item, selected); };
}

/// @brief Replace the renderer used to paint individual list entries.
/// @details The new renderer is stored by value and invoked for every row during
///          @ref paint().  Callers can supply lambdas capturing external state
///          to implement custom highlighting or metadata columns.
/// @param r Callable invoked to render each row.
void ListView::setRenderer(ItemRenderer r) {
    renderer_ = std::move(r);
}

/// @brief Draw the visible list entries into the screen buffer.
/// @details Iterates over the item vector until either the list ends or the
///          widget's height is exhausted.  Each row delegates to the configured
///          renderer, passing along the theme and current selection flag.
/// @param sb Screen buffer receiving the rendered rows.
void ListView::paint(render::ScreenBuffer &sb) {
    for (std::size_t i = 0; i < items_.size() && static_cast<int>(i) < rect_.h; ++i) {
        renderer_(sb, rect_.y + static_cast<int>(i), items_[i], selected_[i], theme_);
    }
}

/// @brief Process navigation keystrokes and update selection state.
/// @details Supports arrow key movement with optional modifiers:
///          * Ctrl+Arrow moves the cursor without altering selection.
///          * Shift+Arrow extends a range selection from the anchor to the new
///            cursor position.
///          * Plain Arrow selects only the focused item.
///          When the list is empty the event is ignored.
/// @param ev UI event describing the keyboard input.
/// @return True when the event was consumed.
bool ListView::onEvent(const ui::Event &ev) {
    if (items_.empty()) {
        return false;
    }
    const auto &k = ev.key;
    if (k.code == term::KeyEvent::Code::Up) {
        if (cursor_ > 0) {
            --cursor_;
        }
    } else if (k.code == term::KeyEvent::Code::Down) {
        if (cursor_ + 1 < static_cast<int>(items_.size())) {
            ++cursor_;
        }
    } else {
        return false;
    }

    if (k.mods & term::KeyEvent::Mods::Ctrl) {
        return true;
    }
    if (k.mods & term::KeyEvent::Mods::Shift) {
        clearSelection();
        selectRange(anchor_, cursor_);
        return true;
    }
    clearSelection();
    anchor_ = cursor_;
    selected_[cursor_] = true;
    return true;
}

/// @brief List views require focus to process navigation keystrokes.
/// @return Always true so keyboard navigation can reach the widget.
bool ListView::wantsFocus() const {
    return true;
}

/// @brief Return the indices of all selected rows.
/// @details Iterates over the selection flags and collects indices for entries
///          marked true.  The resulting vector is ordered according to the
///          underlying item list.
/// @return Vector containing selected item indices.
std::vector<int> ListView::selection() const {
    std::vector<int> out;
    for (std::size_t i = 0; i < selected_.size(); ++i) {
        if (selected_[i]) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

/// @brief Default renderer that paints a cursor arrow and the row text.
/// @details Uses the accent theme role for the arrow indicator and the normal
///          role for the remainder of the line.  Excess horizontal space is
///          filled with spaces to avoid artefacts from previous frames.
/// @param sb Screen buffer receiving the rendered characters.
/// @param row Absolute buffer row at which to render.
/// @param item Text of the list entry.
/// @param selected Whether the entry is part of the current selection.
void ListView::defaultRender(render::ScreenBuffer &sb,
                             int row,
                             const std::string &item,
                             bool selected) {
    const auto &sel = theme_.style(style::Role::Accent);
    const auto &nor = theme_.style(style::Role::Normal);
    int x = rect_.x;
    auto &pref = sb.at(row, x);
    pref.ch = selected ? U'>' : U' ';
    pref.style = sel;
    int maxw = rect_.w - 1;
    for (int i = 0; i < maxw; ++i) {
        auto &c = sb.at(row, x + 1 + i);
        if (i < static_cast<int>(item.size())) {
            c.ch = static_cast<char32_t>(item[i]);
        } else {
            c.ch = U' ';
        }
        c.style = nor;
    }
}

/// @brief Reset all selection flags to the cleared state.
/// @details Implemented as a call to `std::fill` to keep complexity linear in
///          the number of items.
void ListView::clearSelection() {
    std::fill(selected_.begin(), selected_.end(), false);
}

/// @brief Select a contiguous range of items between two indices.
/// @details Accepts indices in any order, swapping when necessary, and clamps
///          the upper bound to the list length.  Negative indices are ignored so
///          callers can pass -1 to represent "no anchor" without extra checks.
/// @param a One endpoint of the selection range.
/// @param b The other endpoint of the selection range.
void ListView::selectRange(int a, int b) {
    if (a > b) {
        std::swap(a, b);
    }
    for (int i = a; i <= b && i < static_cast<int>(selected_.size()); ++i) {
        if (i >= 0) {
            selected_[i] = true;
        }
    }
}

} // namespace zanna::tui::widgets
