//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares an incremental command-search and execution palette.
/// @details The focusable widget filters a borrowed keymap as the user types,
///          paints matching command names, executes the leading match on Enter,
///          and supports dismissal on Escape.
//
// As the user types, the palette filters commands by name, displaying
// matching results below the query line. Pressing Enter executes the
// top-matching command. Pressing Escape dismisses the palette.
//
// Key invariants:
//   - The palette borrows the Keymap for command lookup (not owned).
//   - Results are updated incrementally as the query changes.
//   - The palette always wants focus to capture typing.
//
// Ownership: CommandPalette borrows Keymap and Theme references. It owns
// the query string and filtered results vector.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <vector>

#include "tui/input/keymap.hpp"
#include "tui/style/theme.hpp"
#include "tui/ui/widget.hpp"

namespace zanna::tui::widgets {

/// @brief Incremental search command palette for discovering and executing commands.
/// @details Displays a text input field and a filtered list of registered commands.
///          As the user types, commands are filtered by name. Pressing Enter executes
///          the selected command. Designed to be shown as a modal overlay.
class CommandPalette : public ui::Widget {
  public:
    /// @brief Construct a command palette bound to a keymap and theme.
    /// @param km Keymap containing registered commands to search. Must outlive the widget.
    /// @param theme Theme providing colors for rendering. Must outlive the widget.
    CommandPalette(input::Keymap &km, const style::Theme &theme);

    /// @brief Paint query and filtered commands.
    /// @param sb Screen buffer receiving the palette contents.
    void paint(render::ScreenBuffer &sb) override;

    /// @brief Handle typing and Enter to execute command.
    /// @param ev Input event containing navigation, editing, or activation keys.
    /// @return true when the palette consumed the event.
    bool onEvent(const ui::Event &ev) override;

    /// @brief Palette requires focus for typing.
    /// @return Always true.
    [[nodiscard]] bool wantsFocus() const override;

  private:
    input::Keymap &km_;             ///< Borrowed command registry and executor.
    const style::Theme &theme_;     ///< Borrowed render palette.
    std::string query_{};           ///< Current incremental search text.
    std::vector<input::CommandId> results_{}; ///< Matching command ids in display order.

    /// @brief Rebuild @c results_ from the current query and registered commands.
    void update();
};

} // namespace zanna::tui::widgets
