//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the command palette widget that lets users filter and execute
// registered commands.  The widget maintains a searchable list derived from the
// active keymap and renders the top matches into the TUI viewport.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the interactive command palette widget.
/// @details The widget captures keystrokes while focused, updates a filtered
///          list of command identifiers, and paints the results together with
///          the query prompt.  Executing a command delegates back to the
///          keymap, keeping ownership of callbacks centralised.

#include "tui/widgets/command_palette.hpp"

#include "tui/render/screen.hpp"
#include "tui/util/string.hpp"

namespace zanna::tui::widgets {
using zanna::tui::term::KeyEvent;

/// @brief Construct a command palette bound to an existing keymap and theme.
/// @details Stores references to collaborators and immediately generates the
///          initial result list so the widget paints correctly before receiving
///          user input.
/// @param km Borrowed command registry and dispatcher that must outlive the
///        palette.
/// @param theme Borrowed theme that must outlive the palette.
CommandPalette::CommandPalette(input::Keymap &km, const style::Theme &theme)
    : km_(km), theme_(theme) {
    update();
}

/// @brief Command palette must hold focus to accept incremental query input.
/// @details Returning true ensures the application routes keystrokes directly to
///          the widget whenever it is active.
/// @return Always @c true.
bool CommandPalette::wantsFocus() const {
    return true;
}

/// @brief Rebuild the filtered command list from the current query string.
/// @details Normalises both the query and candidate command names to lowercase
///          so matching becomes case-insensitive.  Commands whose name contains
///          the query as a substring are kept in insertion order.
void CommandPalette::update() {
    results_.clear();
    std::string q = util::toLower(query_);
    for (const auto &cmd : km_.commands()) {
        std::string name = util::toLower(cmd.name);
        if (q.empty() || name.find(q) != std::string::npos) {
            results_.push_back(cmd.id);
        }
    }
}

/// @brief Handle keyboard input for editing the query and triggering commands.
/// @details Supports backspace, enter, and printable ASCII characters.  Enter
///          executes the first match, while typing adds characters to the query
///          and re-filters the result list.  Unhandled keys bubble up to callers.
/// @param ev Input event to interpret.
/// @return @c true for Backspace, Enter, or accepted printable ASCII input;
///         otherwise @c false.
bool CommandPalette::onEvent(const ui::Event &ev) {
    using Code = KeyEvent::Code;
    if (ev.key.code == Code::Backspace) {
        if (!query_.empty()) {
            query_.pop_back();
            update();
        }
        return true;
    }
    if (ev.key.code == Code::Enter) {
        if (!results_.empty()) {
            km_.execute(results_.front());
        }
        return true;
    }
    if (ev.key.code == Code::Unknown && ev.key.codepoint >= 32 && ev.key.codepoint <= 126) {
        query_.push_back(static_cast<char>(ev.key.codepoint));
        update();
        return true;
    }
    return false;
}

/// @brief Paint the command palette contents into the provided screen buffer.
/// @details Clears the widget's rectangle, renders the prompt prefixed with a
///          colon, and lists the currently matched command names.  Rows beyond
///          the widget height are clipped.
/// @param sb Screen buffer that receives the prompt and filtered command list.
void CommandPalette::paint(render::ScreenBuffer &sb) {
    const auto &st = theme_.style(style::Role::Normal);
    for (int y = 0; y < rect_.h; ++y) {
        for (int x = 0; x < rect_.w; ++x) {
            auto &cell = sb.at(rect_.y + y, rect_.x + x);
            cell.ch = U' ';
            cell.style = st;
        }
    }
    std::string header = ":" + query_;
    for (int x = 0; x < rect_.w && x < static_cast<int>(header.size()); ++x) {
        auto &cell = sb.at(rect_.y, rect_.x + x);
        cell.ch = static_cast<char32_t>(header[x]);
        cell.style = st;
    }
    int row = 1;
    for (const auto &id : results_) {
        if (row >= rect_.h) {
            break;
        }
        if (const auto *cmd = km_.find(id)) {
            for (int x = 0; x < rect_.w && x < static_cast<int>(cmd->name.size()); ++x) {
                auto &cell = sb.at(rect_.y + row, rect_.x + x);
                cell.ch = static_cast<char32_t>(cmd->name[x]);
                cell.style = st;
            }
        }
        ++row;
    }
}

} // namespace zanna::tui::widgets
