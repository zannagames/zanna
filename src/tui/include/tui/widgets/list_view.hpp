//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares a scrollable, selectable TUI string-list widget.
/// @details ListView owns items and per-item selection state, supports cursor
///          and range selection, and delegates row painting to an optional
///          caller-provided renderer.
//
// The widget supports single and range selection: clicking or pressing
// Enter selects a single item, while Shift+arrow keys extend the
// selection range. A custom ItemRenderer can be installed to control
// how each item is painted.
//
// Key invariants:
//   - The cursor index is always within [0, items_.size()).
//   - Selection state is maintained per-item via a boolean vector.
//   - The default renderer highlights the cursor and selected items.
//
// Ownership: ListView owns items and selection state by value. It
// borrows the Theme reference (must outlive the widget). The optional
// ItemRenderer is owned via std::function.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

/// @brief Vertical scrollable list widget with keyboard navigation and selection.
/// @details Displays a list of string items with cursor-based navigation (Up/Down),
///          single selection (Enter), and range selection (Shift+arrows). Supports
///          custom item rendering via an installable ItemRenderer callback.
class ListView : public ui::Widget {
  public:
    /// @brief Callback used to paint one list row.
    /// @details Receives the screen buffer, absolute row, item text, selection
    ///          state, and borrowed theme.
    /// @param sb Screen buffer receiving the row.
    /// @param row Absolute screen row.
    /// @param item Item label.
    /// @param selected Whether the row is selected.
    /// @param theme Borrowed render palette.
    using ItemRenderer = std::function<void(render::ScreenBuffer &,
                                            int row,
                                            const std::string &item,
                                            bool selected,
                                            const style::Theme &theme)>;

    /// @brief Construct list view with items and theme.
    /// @param items Owned list entries in display order.
    /// @param theme Borrowed render palette that must outlive the widget.
    ListView(std::vector<std::string> items, const style::Theme &theme);

    /// @brief Set custom item renderer.
    /// @param r Owned callback replacing the default row renderer.
    void setRenderer(ItemRenderer r);

    /// @brief Paint list items.
    /// @param sb Screen buffer receiving visible rows.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle navigation and selection keys.
    /// @param ev Input event to interpret.
    /// @return True if event consumed.
    bool onEvent(const ui::Event &ev) override;

    /// @brief List wants focus for keyboard navigation.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

    /// @brief Retrieve selected item indices.
    /// @return Zero-based selected indices in ascending order.
    [[nodiscard]] std::vector<int> selection() const;

  private:
    std::vector<std::string> items_{}; ///< Owned entries in display order.
    std::vector<bool> selected_{};     ///< Selection flag parallel to @c items_.
    ItemRenderer renderer_{};          ///< Optional owned custom row painter.
    const style::Theme &theme_;        ///< Borrowed render palette.
    int cursor_{0};                    ///< Current item index.
    int anchor_{0};                    ///< Range-selection anchor index.

    /// @brief Paint one row using built-in text and selection styles.
    /// @param sb Screen buffer receiving the row.
    /// @param row Absolute target row.
    /// @param item Item text to clip and render.
    /// @param selected Whether the row uses the selected style.
    void defaultRender(render::ScreenBuffer &sb, int row, const std::string &item, bool selected);

    /// @brief Clear every per-item selection flag.
    void clearSelection();

    /// @brief Select the inclusive item-index range between two endpoints.
    /// @param a First endpoint.
    /// @param b Second endpoint.
    void selectRange(int a, int b);
};

} // namespace zanna::tui::widgets
